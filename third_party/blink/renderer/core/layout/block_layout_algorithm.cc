// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/layout/block_layout_algorithm.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

#include "third_party/blink/renderer/core/frame/web_feature.h"
#include "third_party/blink/renderer/core/html/forms/html_input_element.h"
#include "third_party/blink/renderer/core/layout/block_child_iterator.h"
#include "third_party/blink/renderer/core/layout/block_layout_algorithm_utils.h"
#include "third_party/blink/renderer/core/layout/box_fragment_builder.h"
#include "third_party/blink/renderer/core/layout/column_spanner_path.h"
#include "third_party/blink/renderer/core/layout/constraint_space.h"
#include "third_party/blink/renderer/core/layout/constraint_space_builder.h"
#include "third_party/blink/renderer/core/layout/early_break.h"
#include "third_party/blink/renderer/core/layout/floats_utils.h"
#include "third_party/blink/renderer/core/layout/fragmentation_utils.h"
#include "third_party/blink/renderer/core/layout/inline/fit_text_scale.h"
#include "third_party/blink/renderer/core/layout/inline/fit_text_utils.h"
#include "third_party/blink/renderer/core/layout/inline/inline_cursor.h"
#include "third_party/blink/renderer/core/layout/inline/inline_node.h"
#include "third_party/blink/renderer/core/layout/inline/physical_line_box_fragment.h"
#include "third_party/blink/renderer/core/layout/inline/ruby_utils.h"
#include "third_party/blink/renderer/core/layout/layout_object.h"
#include "third_party/blink/renderer/core/layout/layout_result.h"
#include "third_party/blink/renderer/core/layout/length_utils.h"
#include "third_party/blink/renderer/core/layout/list/unpositioned_list_marker.h"
#include "third_party/blink/renderer/core/layout/logical_box_fragment.h"
#include "third_party/blink/renderer/core/layout/logical_fragment.h"
#include "third_party/blink/renderer/core/layout/physical_box_fragment.h"
#include "third_party/blink/renderer/core/layout/positioned_float.h"
#include "third_party/blink/renderer/core/layout/space_utils.h"
#include "third_party/blink/renderer/core/layout/table/table_layout_utils.h"
#include "third_party/blink/renderer/core/layout/unpositioned_float.h"
#include "third_party/blink/renderer/core/mathml/mathml_element.h"
#include "third_party/blink/renderer/core/mathml/mathml_table_cell_element.h"
#include "third_party/blink/renderer/core/mathml_names.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/platform/heap/collection_support/clear_collection_scope.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"

// ------ ipid logging START ------
#include "third_party/blink/renderer/core/layout/ipid_debug_layout_str_utils.h"
#include "third_party/blink/renderer/platform/ipid_logging/ipid_depth_logging.h"
// ------ ipid logging END ------

namespace blink {
namespace {

bool HasLineEvenIfEmpty(LayoutBox* box) {
  // Note: We should reduce calling |LayoutBlock::HasLineIfEmpty()|, because
  // it calls slow function |IsRootEditableElement()|.
  LayoutBlockFlow* const block_flow = DynamicTo<LayoutBlockFlow>(box);
  if (!block_flow)
    return false;
  // Note: |block_flow->NeedsCollectInline()| is true after removing all
  // children from block[1].
  // [1] editing/inserting/insert_after_delete.html
  if (!block_flow->FirstChild()) {
    // Note: |block_flow->ChildrenInline()| can be both true or false:
    //  - true: just after construction, <div></div>
    //  - true: one of child is inline them remove all, <div>abc</div>
    //  - false: all children are block then remove all, <div><p></p></div>
    return block_flow->HasLineIfEmpty();
  }
  if (block_flow->ChildrenInline()) {
    return block_flow->HasLineIfEmpty() &&
           InlineNode(block_flow).IsBlockLevel();
  }
  const LayoutBlockFlow* fragmentation_context_root = nullptr;
  if (block_flow->IsMulticolContainer()) {
    fragmentation_context_root = block_flow;
  }
  if (fragmentation_context_root) {
    DCHECK(!fragmentation_context_root->ChildrenInline());
    for (const auto* child = fragmentation_context_root->FirstChild(); child;
         child = child->NextSibling()) {
      if (child->IsInline()) {
        // Note: |LayoutOutsideListMarker| is out-of-flow for the tree
        // building purpose in |LayoutBlockFlow::AddChild()|.
        // |MultiColumnRenderingTest.ListItem| reaches here.
        DCHECK(child->IsLayoutOutsideListMarker()) << child;
        return false;
      }
      if (!child->IsFloatingOrOutOfFlowPositioned()) {
        // We reach here when we have in-flow child.
        // <div style="columns: 3"><div style="float:left"><div></div></div>
        return false;
      }
    }
    // There are no children or all children are floating or out of flow
    // positioned.
    return block_flow->HasLineIfEmpty();
  }
  return false;
}

inline bool IsLastInflowChild(const LayoutBox& box) {
  for (const LayoutObject* next = box.NextSibling(); next;
       next = next->NextSibling()) {
    if (!next->IsFloatingOrOutOfFlowPositioned()) {
      return false;
    }
  }
  return true;
}

inline const LayoutResult* LayoutBlockChild(
    const ConstraintSpace& space,
    const BreakToken* break_token,
    const EarlyBreak* early_break,
    const ColumnSpannerPath* column_spanner_path,
    BlockNode* node) {
  const EarlyBreak* early_break_in_child = nullptr;
  if (early_break) [[unlikely]] {
    early_break_in_child = EnterEarlyBreakInChild(*node, *early_break);
  }
  column_spanner_path = FollowColumnSpannerPath(column_spanner_path, *node);
  return node->Layout(space, To<BlockBreakToken>(break_token),
                      early_break_in_child, column_spanner_path);
}

inline const LayoutResult* LayoutInflow(
    const ConstraintSpace& space,
    const BreakToken* break_token,
    const EarlyBreak* early_break,
    const ColumnSpannerPath* column_spanner_path,
    LayoutInputNode* node,
    InlineChildLayoutContext* context) {
  if (auto* inline_node = DynamicTo<InlineNode>(node)) {
    return inline_node->Layout(space, break_token, column_spanner_path,
                               context);
  }
  return LayoutBlockChild(space, break_token, early_break, column_spanner_path,
                          To<BlockNode>(node));
}

AdjoiningObjectTypes ToAdjoiningObjectTypes(EClear clear) {
  switch (clear) {
    default:
      NOTREACHED();
    case EClear::kNone:
      return kAdjoiningNone;
    case EClear::kLeft:
      return kAdjoiningFloatLeft;
    case EClear::kRight:
      return kAdjoiningFloatRight;
    case EClear::kBoth:
      return kAdjoiningFloatBoth;
  };
}

// Return true if a child is to be cleared past adjoining floats. These are
// floats that would otherwise (if 'clear' were 'none') be pulled down by the
// BFC block offset of the child. If the child is to clear floats, though, we
// obviously need separate the child from the floats and move it past them,
// since that's what clearance is all about. This means that if we have any such
// floats to clear, we know for sure that we get clearance, even before layout.
inline bool HasClearancePastAdjoiningFloats(
    AdjoiningObjectTypes adjoining_object_types,
    const ComputedStyle& child_style,
    const ComputedStyle& cb_style) {
  return ToAdjoiningObjectTypes(child_style.Clear(cb_style)) &
         adjoining_object_types;
}

// Adjust BFC block offset for clearance, if applicable. Return true of
// clearance was applied.
//
// Clearance applies either when the BFC block offset calculated simply isn't
// past all relevant floats, *or* when we have already determined that we're
// directly preceded by clearance.
//
// The latter is the case when we need to force ourselves past floats that would
// otherwise be adjoining, were it not for the predetermined clearance.
// Clearance inhibits margin collapsing and acts as spacing before the
// block-start margin of the child. It needs to be exactly what takes the
// block-start border edge of the cleared block adjacent to the block-end outer
// edge of the "bottommost" relevant float.
//
// We cannot reliably calculate the actual clearance amount at this point,
// because 1) this block right here may actually be a descendant of the block
// that is to be cleared, and 2) we may not yet have separated the margin before
// and after the clearance. None of this matters, though, because we know where
// to place this block if clearance applies: exactly at the ConstraintSpace's
// ClearanceOffset().
bool ApplyClearance(const ConstraintSpace& constraint_space,
                    LayoutUnit* bfc_block_offset) {
  if (constraint_space.HasClearanceOffset() &&
      *bfc_block_offset < constraint_space.ClearanceOffset()) {
    *bfc_block_offset = constraint_space.ClearanceOffset();
    return true;
  }
  return false;
}

LayoutUnit LogicalFromBfcLineOffset(LayoutUnit child_bfc_line_offset,
                                    LayoutUnit parent_bfc_line_offset,
                                    LayoutUnit child_inline_size,
                                    LayoutUnit parent_inline_size,
                                    TextDirection direction) {
  // We need to respect the current text direction to calculate the logical
  // offset correctly.
  LayoutUnit relative_line_offset =
      child_bfc_line_offset - parent_bfc_line_offset;

  LayoutUnit inline_offset =
      direction == TextDirection::kLtr
          ? relative_line_offset
          : parent_inline_size - relative_line_offset - child_inline_size;

  return inline_offset;
}

LogicalOffset LogicalFromBfcOffsets(const BfcOffset& child_bfc_offset,
                                    const BfcOffset& parent_bfc_offset,
                                    LayoutUnit child_inline_size,
                                    LayoutUnit parent_inline_size,
                                    TextDirection direction) {
  LayoutUnit inline_offset = LogicalFromBfcLineOffset(
      child_bfc_offset.line_offset, parent_bfc_offset.line_offset,
      child_inline_size, parent_inline_size, direction);

  return {inline_offset,
          child_bfc_offset.block_offset - parent_bfc_offset.block_offset};
}

ItemPosition WebkitTextToItemPosition(ETextAlign text_align) {
  switch (text_align) {
    case ETextAlign::kWebkitLeft:
      return ItemPosition::kLeft;
    case ETextAlign::kWebkitCenter:
      return ItemPosition::kCenter;
    case ETextAlign::kWebkitRight:
      return ItemPosition::kRight;
    default:
      // Ignore non -webkit- values.
      return ItemPosition::kNormal;
  }
}

// Handle text-align:-webkit-* and justify-self.
template <typename ChildInlineSizeFunc>
LayoutUnit WebkitTextAlignAndJustifySelfOffset(
    const ComputedStyle& child_style,
    const ComputedStyle& style,
    LayoutUnit available_space,
    const BoxStrut& margins,
    const ChildInlineSizeFunc& child_inline_size_func) {
  DCHECK(!child_style.MarginInlineStartUsing(style).IsAuto());
  DCHECK(!child_style.MarginInlineEndUsing(style).IsAuto());

  const StyleSelfAlignmentData alignment_data = child_style.ResolvedJustifySelf(
      {ItemPosition::kNormal, OverflowAlignment::kDefault}, &style);
  ItemPosition justify_self = alignment_data.GetPosition();
  OverflowAlignment safe = OverflowAlignment::kSafe;
  if (justify_self != ItemPosition::kNormal) {
    safe = alignment_data.Overflow();
  } else {
    justify_self = WebkitTextToItemPosition(style.GetTextAlign());
  }
  auto FreeSpace = [&]() -> LayoutUnit {
    const LayoutUnit free_space =
        available_space - child_inline_size_func() - margins.InlineSum();
    return safe == OverflowAlignment::kSafe ? free_space.ClampNegativeToZero()
                                            : free_space;
  };

  auto self_start_end_converter = [&]() -> LogicalToLogical<LayoutUnit> {
    const LayoutUnit free_space = FreeSpace();
    return LogicalToLogical<LayoutUnit>(
        child_style.GetWritingDirection(), style.GetWritingDirection(),
        /* inline_start */ LayoutUnit(), /* inline_end */ free_space,
        /* block_start */ LayoutUnit(), /* block_end */ free_space);
  };

  bool is_rtl = IsRtl(style.Direction());
  switch (justify_self) {
    case ItemPosition::kLeft:
      return is_rtl ? FreeSpace() : LayoutUnit();
    case ItemPosition::kCenter:
      return FreeSpace() / 2;
    case ItemPosition::kRight:
      return is_rtl ? LayoutUnit() : FreeSpace();
    case ItemPosition::kFlexStart:
    case ItemPosition::kStart:
      return LayoutUnit();
    case ItemPosition::kFlexEnd:
    case ItemPosition::kEnd:
      return FreeSpace();
    case ItemPosition::kSelfStart:
      return self_start_end_converter().InlineStart();
    case ItemPosition::kSelfEnd:
      return self_start_end_converter().InlineEnd();
    default:
      return LayoutUnit();
  }
}

}  // namespace

BlockLayoutAlgorithm::BlockLayoutAlgorithm(const LayoutAlgorithmParams& params)
    : LayoutAlgorithm(params),
      previous_result_(params.previous_result),
      column_spanner_path_(params.column_spanner_path),
      line_clamp_data_(params.space.GetLineClampData()),
      fit_all_lines_(false),
      is_resuming_(IsBreakInside(params.break_token)),
      abort_when_bfc_block_offset_updated_(false),
      has_break_opportunity_before_next_child_(false) {
  container_builder_.SetExclusionSpace(params.space.GetExclusionSpace());

  child_percentage_size_ = CalculateChildPercentageSize(
      GetConstraintSpace(), Node(), ChildAvailableSize());
  replaced_child_percentage_size_ = CalculateReplacedChildPercentageSize(
      GetConstraintSpace(), Node(), ChildAvailableSize(),
      BorderScrollbarPadding(), BorderPadding());

  // If |this| is a list item, keep track of the unpositioned list marker in
  // |container_builder_|.
  if (const BlockNode marker_node = Node().ListMarkerBlockNodeIfListItem()) {
    if (ShouldPlaceUnpositionedListMarker() &&
        !marker_node.ListMarkerOccupiesWholeLine() &&
        (!GetBreakToken() || GetBreakToken()->HasUnpositionedListMarker())) {
      container_builder_.SetUnpositionedListMarker(
          UnpositionedListMarker(marker_node));
    }
  }

  container_builder_.SetInitialTextBoxTrim();
}

// Define the destructor here, so that we can forward-declare more in the
// header.
BlockLayoutAlgorithm::~BlockLayoutAlgorithm() = default;

void BlockLayoutAlgorithm::SetupRelayoutData(
    const BlockLayoutAlgorithm& previous,
    RelayoutType relayout_type) {
  LayoutAlgorithm::SetupRelayoutData(previous, relayout_type);

  column_spanner_path_ = previous.column_spanner_path_;

  if (relayout_type == kRelayoutIgnoringLineClamp) {
    line_clamp_data_.data.state = LineClampData::kDisabled;
    line_clamp_data_.ignore_line_clamp = true;
  } else if (relayout_type == kRelayoutClampingByLines) {
    line_clamp_data_.data.state = LineClampData::kClampByLines;
    line_clamp_data_.data.lines_until_clamp =
        line_clamp_data_.initial_lines_until_clamp =
            previous.line_clamp_data_.data.lines_until_clamp;
  } else if (relayout_type == kRelayoutClampingAfterLayoutObject) {
    line_clamp_data_.data.state = LineClampData::kClampAfterLayoutObject;
    line_clamp_data_.data.clamp_after_layout_object =
        previous.line_clamp_data_.last_layout_object;
  } else if (previous.line_clamp_data_.data.IsClampByLines()) {
    line_clamp_data_.data.state = LineClampData::kClampByLines;
    line_clamp_data_.data.lines_until_clamp =
        line_clamp_data_.initial_lines_until_clamp =
            previous.line_clamp_data_.initial_lines_until_clamp;
  }

  if (relayout_type == kRelayoutForTextBoxTrim) {
    DCHECK(previous.last_non_empty_inflow_child_);
    // If there is at least one non-empty inflow child, re-layout by applying
    // the `text-box-trim: end` to the `last_non_empty_inflow_child_`.
    override_text_box_trim_end_child_ = previous.last_non_empty_inflow_child_;
    override_text_box_trim_end_break_token_ =
        previous.last_non_empty_break_token_;
  } else {
    override_text_box_trim_end_child_ =
        previous.override_text_box_trim_end_child_;
    override_text_box_trim_end_break_token_ =
        previous.override_text_box_trim_end_break_token_;
    container_builder_.SetShouldTextBoxTrimNodeEnd(
        previous.container_builder_.ShouldTextBoxTrimNodeEnd());

    if (relayout_mode_ & kRelayoutForTextBoxTrim) {
      // Text box end trimming was done in a previous relayout pass. Make sure
      // that it's re-applied.
      override_text_box_trim_end_child_ =
          previous.override_text_box_trim_end_child_;
      override_text_box_trim_end_break_token_ =
          previous.override_text_box_trim_end_break_token_;
    }
  }
}

void BlockLayoutAlgorithm::SetBoxType(PhysicalFragment::BoxType type) {
  container_builder_.SetBoxType(type);
}

MinMaxSizesResult BlockLayoutAlgorithm::ComputeMinMaxSizes(
    const MinMaxSizesFloatInput& float_input) {
  IpidDepthLog ipid_depth_log("BlockLayoutAlgorithm::ComputeMinMaxSizes");
  ipid_depth_log.FPrint(
      "正在计算元素 {} 的固有宽度。浮动输入状态：左侧浮动宽度 "
      "{}px，右侧浮动宽度 {}px。",
      ipid::GetNodeStr(Node()), float_input.float_left_inline_size,
      float_input.float_right_inline_size);

  if (auto result = CalculateMinMaxSizesIgnoringChildren(
          node_, BorderScrollbarPadding())) {
    ipid_depth_log.FPrint(
        "元素 {} 无需考虑子元素即可确定固有宽度，直接返回结果 {}。",
        ipid::GetNodeStr(Node()), result->sizes);
    return *result;
  }

  ipid_depth_log.FPrint("元素 {} 需要遍历子元素来计算固有宽度。",
                        ipid::GetNodeStr(Node()));

  MinMaxSizes sizes;
  bool depends_on_block_constraints = false;

  const TextDirection direction = Style().Direction();
  LayoutUnit float_left_inline_size = float_input.float_left_inline_size;
  LayoutUnit float_right_inline_size = float_input.float_right_inline_size;

  ipid_depth_log.FPrint(
      "初始化固有宽度计算状态：当前固有宽度 {}，文本方向 "
      "{}，浮动追踪器状态：左侧 {}px，右侧 {}px。",
      sizes, (direction == TextDirection::kLtr ? "从左到右" : "从右到左"),
      float_left_inline_size, float_right_inline_size);

  for (LayoutInputNode child = Node().FirstChild(); child;
       child = child.NextSibling()) {
    // We don't check IsRubyText() here intentionally. RubyText width should
    // affect this width.
    if (child.IsOutOfFlowPositioned() ||
        (child.IsColumnSpanAll() && GetConstraintSpace().IsInColumnBfc())) {
      ipid_depth_log.FPrint("跳过元素 {}：{}", ipid::GetNodeStr(child),
                            child.IsOutOfFlowPositioned()
                                ? "绝对定位元素不影响父元素固有宽度"
                                : "多列布局中的跨列元素在BFC中不影响固有宽度");
      continue;
    }

    if (child.IsTextControlPlaceholder()) {
      if (Style().ApplyControlFixedSize(Node().GetDOMNode())) {
        ipid_depth_log.FPrint(
            "跳过元素 {}：表单控件占位符在固定尺寸控件中不影响固有宽度。",
            ipid::GetNodeStr(child));
        continue;
      }
    }

    const ComputedStyle& child_style = child.Style();
    const EClear child_clear = child_style.Clear(Style());
    bool child_is_new_fc = child.CreatesNewFormattingContext();

    std::string ipid_clear_desc = ipid::GetComputedStyleEClearStr(child_clear);
    ipid_depth_log.FPrint("开始处理子元素 {}。子元素属性：{}，{}。",
                          ipid::GetNodeStr(child), ipid_clear_desc,
                          child_is_new_fc
                              ? "创建新的格式化上下文（BFC/IFC/FFC等）"
                              : "不创建新格式化上下文");

    // Conceptually floats and a single new-FC would just get positioned on a
    // single "line". If there is a float/new-FC with clearance, this creates a
    // new "line", resetting the appropriate float size trackers.
    //
    // Both of the float size trackers get reset for anything that isn't a float
    // (inflow and new-FC) at the end of the loop, as this creates a new "line".
    if (child.IsFloating() || child_is_new_fc) {
      LayoutUnit float_inline_size =
          float_left_inline_size + float_right_inline_size;

      ipid_depth_log.FPrint(
          "元素 {} 是{}。当前行上的总浮动宽度为 {}px（左侧 {}px + 右侧 "
          "{}px）。",
          ipid::GetNodeStr(child),
          child.IsFloating() ? "浮动元素" : "新格式化上下文元素",
          float_inline_size, float_left_inline_size, float_right_inline_size);

      if (child_clear != EClear::kNone) {
        ipid_depth_log.FPrint(
            "由于子元素 {} 设置了 "
            "{}，需要清除浮动并换到新行。将当前行的总浮动宽度 {}px "
            "更新到最大固有宽度中。更新前最大宽度：{}px。",
            ipid::GetNodeStr(child), ipid_clear_desc, float_inline_size,
            sizes.max_size);
        sizes.max_size = std::max(sizes.max_size, float_inline_size);
        ipid_depth_log.FPrint("更新后最大宽度：{}px。", sizes.max_size);
      }

      if (child_clear == EClear::kBoth || child_clear == EClear::kLeft) {
        ipid_depth_log.FPrint(
            "由于子元素设置了 {}，重置左侧浮动追踪器从 {}px 到 0px。",
            ipid_clear_desc, float_left_inline_size);
        float_left_inline_size = LayoutUnit();
      }

      if (child_clear == EClear::kBoth || child_clear == EClear::kRight) {
        ipid_depth_log.FPrint(
            "由于子元素设置了 {}，重置右侧浮动追踪器从 {}px 到 0px。",
            ipid_clear_desc, float_right_inline_size);
        float_right_inline_size = LayoutUnit();
      }
    }

    MinMaxSizesFloatInput child_float_input;
    if (child.IsInline() || child.IsAnonymousBlockFlow()) {
      child_float_input.float_left_inline_size = float_left_inline_size;
      child_float_input.float_right_inline_size = float_right_inline_size;
      ipid_depth_log.FPrint(
          "元素 {} 是{}，将当前浮动状态传递给子元素：左侧 {}px，右侧 {}px。",
          ipid::GetNodeStr(child),
          child.IsInline() ? "内联元素"
                           : "匿名块流元素（包含内联内容的块级盒子）",
          float_left_inline_size, float_right_inline_size);
    } else {
      ipid_depth_log.FPrint("元素 {} 不是内联或匿名块流元素，不传递浮动状态。",
                            ipid::GetNodeStr(child));
    }

    MinMaxConstraintSpaceBuilder builder(GetConstraintSpace(), Style(), child,
                                         child_is_new_fc);
    builder.SetAvailableBlockSize(ChildAvailableSize().block_size);
    LayoutUnit child_percentage_block_size =
        PercentageSizeForChild(child).block_size;
    builder.SetPercentageResolutionBlockSize(child_percentage_block_size);
    // Pass the replaced %-size down to inline layout.
    if ((child.IsAnonymousBlockFlow() || child.IsInline()) &&
        replaced_child_percentage_size_.block_size !=
            child_percentage_size_.block_size) {
      builder.SetReplacedChildPercentageResolutionBlockSize(
          replaced_child_percentage_size_.block_size);
      ipid_depth_log.FPrint("为元素 {} 设置特殊的替换元素百分比高度解析尺寸。",
                            ipid::GetNodeStr(child));
    }
    const auto space = builder.ToConstraintSpace();

    ipid_depth_log.FPrint(
        "为子元素 {} 创建了约束空间：可用高度 {}px，百分比解析高度 {}px。",
        ipid::GetNodeStr(child), ChildAvailableSize().block_size,
        child_percentage_block_size);

    MinMaxSizesResult child_result;
    if (child.IsInline()) {
      // From |BlockLayoutAlgorithm| perspective, we can handle |InlineNode|
      // almost the same as |BlockNode|, because an |InlineNode| includes
      // all inline nodes following |child| and their descendants, and produces
      // an anonymous box that contains all line boxes.
      // |NextSibling| returns the next block sibling, or nullptr, skipping all
      // following inline siblings and descendants.
      ipid_depth_log.FPrint(
          "调用内联元素 {} 的 ComputeMinMaxSizes "
          "方法来计算其固有宽度。注意：这个 InlineNode "
          "包含了所有紧跟在后的内联元素及其后代。",
          ipid::GetNodeStr(child));
      child_result = To<InlineNode>(child).ComputeMinMaxSizes(
          Style().GetWritingMode(), space, child_float_input);
    } else {
      ipid_depth_log.FPrint(
          "调用 ComputeMinAndMaxContentContribution 来计算块级元素 {} "
          "对父元素固有宽度的贡献。",
          ipid::GetNodeStr(child));
      child_result = ComputeMinAndMaxContentContribution(
          Style(), To<BlockNode>(child), space, child_float_input);
    }
    DCHECK_LE(child_result.sizes.min_size, child_result.sizes.max_size)
        << child.ToString();

    ipid_depth_log.FPrint("子元素 {} 的固有宽度计算结果：{}。",
                          ipid::GetNodeStr(child), child_result.sizes);

    // Determine the max inline contribution of the child.
    BoxStrut margins =
        child.IsInline()
            ? BoxStrut()
            : ComputeMarginsFor(space, child_style, GetConstraintSpace());
    LayoutUnit max_inline_contribution;

    if (child.IsInline()) {
      ipid_depth_log.FPrint("元素 {} 是内联元素，不计算 margin。",
                            ipid::GetNodeStr(child));
    } else {
      ipid_depth_log.FPrint("计算块级元素 {} 的 margin：{}。",
                            ipid::GetNodeStr(child),
                            ipid::GetBoxStrutString(margins));
    }

    if (child.IsFloating()) {
      // A float adds to its inline size to the current "line". The new max
      // inline contribution is just the sum of all the floats on that "line".
      LayoutUnit float_inline_size =
          child_result.sizes.max_size + margins.InlineSum();

      ipid_depth_log.FPrint(
          "元素 {} 是浮动元素。其对当前行的宽度贡献 = 固有最大宽度 {}px + "
          "margin 横向值 {}px = {}px。",
          ipid::GetNodeStr(child), child_result.sizes.max_size,
          margins.InlineSum(), float_inline_size);

      // float_inline_size is negative when the float is completely outside of
      // the content area, by e.g., negative margins. Such floats do not affect
      // the content size.
      if (float_inline_size > 0) {
        const char* ipid_float_direction_text =
            ipid::GetComputedStyleEFloatStr(child_style.Floating(Style()));
        if (child_style.Floating(Style()) == EFloat::kLeft) {
          ipid_depth_log.FPrint(
              "由于元素 {} 设置了 {}，将其宽度贡献 {}px "
              "添加到左侧浮动追踪器中。更新前：{}px。",
              ipid::GetNodeStr(child), ipid_float_direction_text,
              float_inline_size, float_left_inline_size);
          float_left_inline_size += float_inline_size;
          ipid_depth_log.FPrint("更新后左侧浮动追踪器：{}px。",
                                float_left_inline_size);
        } else {
          ipid_depth_log.FPrint(
              "由于元素 {} 设置了 {}，将其宽度贡献 {}px "
              "添加到右侧浮动追踪器中。更新前：{}px。",
              ipid::GetNodeStr(child), ipid_float_direction_text,
              float_inline_size, float_right_inline_size);
          float_right_inline_size += float_inline_size;
          ipid_depth_log.FPrint("更新后右侧浮动追踪器：{}px。",
                                float_right_inline_size);
        }
      } else {
        ipid_depth_log.FPrint(
            "浮动元素 {} 的宽度贡献 {}px 为负数或零（可能由负 margin "
            "导致），不影响浮动追踪器。",
            ipid::GetNodeStr(child), float_inline_size);
      }

      max_inline_contribution =
          float_left_inline_size + float_right_inline_size;
      ipid_depth_log.FPrint(
          "当前行的最大内联贡献 = 左侧浮动 {}px + 右侧浮动 {}px = {}px。",
          float_left_inline_size, float_right_inline_size,
          max_inline_contribution);
    } else if (child_is_new_fc) {
      // As floats are line relative, we perform the margin calculations in the
      // line relative coordinate system as well.
      LayoutUnit margin_line_left = margins.LineLeft(direction);
      LayoutUnit margin_line_right = margins.LineRight(direction);

      ipid_depth_log.FPrint(
          "元素 {} "
          "创建新格式化上下文。计算其在浮动环境中的宽度贡献。在行相对坐标系中："
          "margin-line-left = {}px，margin-line-right = {}px。",
          ipid::GetNodeStr(child), margin_line_left, margin_line_right);

      // line_left_inset and line_right_inset are the "distance" from their
      // respective edges of the parent that the new-FC would take. If the
      // margin is positive the inset is just whichever of the floats inline
      // size and margin is larger, and if negative it just subtracts from the
      // float inline size.
      LayoutUnit line_left_inset =
          margin_line_left > LayoutUnit()
              ? std::max(float_left_inline_size, margin_line_left)
              : float_left_inline_size + margin_line_left;

      LayoutUnit line_right_inset =
          margin_line_right > LayoutUnit()
              ? std::max(float_right_inline_size, margin_line_right)
              : float_right_inline_size + margin_line_right;

      ipid_depth_log.FPrint(
          "计算新BFC元素 {} 的左右占用空间：\n"
          "- 左侧：{} = {} \n"
          "- 右侧：{} = {}",
          ipid::GetNodeStr(child),
          (margin_line_left > LayoutUnit()
               ? "正margin，取max(左侧浮动宽度, margin)"
               : "负margin，浮动宽度 + margin"),
          line_left_inset,
          (margin_line_right > LayoutUnit()
               ? "正margin，取max(右侧浮动宽度, margin)"
               : "负margin，浮动宽度 + margin"),
          line_right_inset);

      // The order of operations is important here.
      // If child_result.sizes.max_size is saturated, adding the insets
      // sequentially can result in an DCHECK.
      max_inline_contribution =
          child_result.sizes.max_size + (line_left_inset + line_right_inset);
      ipid_depth_log.FPrint(
          "新BFC元素 {} 的最大内联贡献 = 固有最大宽度 {}px + (左侧占用 {}px + "
          "右侧占用 {}px) = {}px。",
          ipid::GetNodeStr(child), child_result.sizes.max_size, line_left_inset,
          line_right_inset, max_inline_contribution);
    } else {
      // This is just a standard inflow child.
      max_inline_contribution =
          child_result.sizes.max_size + margins.InlineSum();
      ipid_depth_log.FPrint(
          "普通的流内块级元素 {} 的最大内联贡献 = 固有最大宽度 {}px + margin "
          "横向值 {}px = {}px。",
          ipid::GetNodeStr(child), child_result.sizes.max_size,
          margins.InlineSum(), max_inline_contribution);
    }
    LayoutUnit old_max_size = sizes.max_size;
    sizes.max_size = std::max(sizes.max_size, max_inline_contribution);
    if (sizes.max_size != old_max_size) {
      ipid_depth_log.FPrint(
          "更新父元素的固有最大宽度：从 {}px 更新为 {}px（子元素 {} 贡献了 "
          "{}px）。",
          old_max_size, sizes.max_size, ipid::GetNodeStr(child),
          max_inline_contribution);
    }

    // The min inline contribution just assumes that floats are all on their own
    // "line".
    LayoutUnit min_inline_contribution =
        child_result.sizes.min_size + margins.InlineSum();
    LayoutUnit old_min_size = sizes.min_size;
    sizes.min_size = std::max(sizes.min_size, min_inline_contribution);

    ipid_depth_log.FPrint(
        "子元素 {} 的固有最小宽度贡献 = 固有最小宽度 {}px + margin 横向值 {}px "
        "= {}px。",
        ipid::GetNodeStr(child), child_result.sizes.min_size,
        margins.InlineSum(), min_inline_contribution);
    if (sizes.min_size != old_min_size) {
      ipid_depth_log.FPrint("更新父元素的固有最小宽度：从 {}px 更新为 {}px。",
                            old_min_size, sizes.min_size);
    }

    depends_on_block_constraints |= child_result.depends_on_block_constraints;
    if (child_result.depends_on_block_constraints) {
      ipid_depth_log.FPrint(
          "子元素 {} 的固有宽度依赖于块级约束，标记父元素也依赖块级约束。",
          ipid::GetNodeStr(child));
    }

    // Anything that isn't a float will create a new "line" resetting the float
    // size trackers.
    if (!child.IsFloating()) {
      if (float_left_inline_size > LayoutUnit() ||
          float_right_inline_size > LayoutUnit()) {
        ipid_depth_log.FPrint(
            "非浮动元素 {} 创建新行，重置浮动追踪器：左侧从 {}px 重置为 "
            "0px，右侧从 {}px 重置为 0px。",
            ipid::GetNodeStr(child), float_left_inline_size,
            float_right_inline_size);
      }
      float_left_inline_size = LayoutUnit();
      float_right_inline_size = LayoutUnit();
    }
  }

  DCHECK_GE(sizes.min_size, LayoutUnit());
  DCHECK_LE(sizes.min_size, sizes.max_size) << Node().ToString();

  ipid_depth_log.FPrint(
      "子元素遍历完成。在加上 border/scrollbar/padding "
      "之前，计算出的固有宽度为：{}。",
      sizes);

  LayoutUnit border_scrollbar_padding_inline =
      BorderScrollbarPadding().InlineSum();
  sizes += border_scrollbar_padding_inline;

  ipid_depth_log.FPrint(
      "最终结果：固有宽度 {} + border/scrollbar/padding 横向值 {}px = "
      "{}。是否依赖块级约束：{}。",
      ipid::GetMinMaxSizesString(
          MinMaxSizes{sizes.min_size - border_scrollbar_padding_inline,
                      sizes.max_size - border_scrollbar_padding_inline}),
      border_scrollbar_padding_inline, sizes,
      ipid::btos(depends_on_block_constraints));

  return MinMaxSizesResult(sizes, depends_on_block_constraints);
}

LogicalOffset BlockLayoutAlgorithm::CalculateLogicalOffset(
    const LogicalFragment& fragment,
    LayoutUnit child_bfc_line_offset,
    const std::optional<LayoutUnit>& child_bfc_block_offset) {
  LayoutUnit inline_size = container_builder_.InlineSize();
  TextDirection direction = GetConstraintSpace().Direction();

  if (child_bfc_block_offset && container_builder_.BfcBlockOffset()) {
    return LogicalFromBfcOffsets(
        {child_bfc_line_offset, *child_bfc_block_offset}, ContainerBfcOffset(),
        fragment.InlineSize(), inline_size, direction);
  }

  LayoutUnit inline_offset = LogicalFromBfcLineOffset(
      child_bfc_line_offset, container_builder_.BfcLineOffset(),
      fragment.InlineSize(), inline_size, direction);

  // If we've reached here, either the parent, or the child don't have a BFC
  // block-offset yet. Children in this situation are always placed at a
  // logical block-offset of zero.
  return {inline_offset, LayoutUnit()};
}

const LayoutResult* BlockLayoutAlgorithm::Layout() {
  const LayoutResult* result = nullptr;
  // Inline children require an inline child layout context to be
  // passed between siblings. We want to stack-allocate that one, but
  // only on demand, as it's quite big.
  InlineNode inline_child(nullptr);
  if (Node().IsInlineFormattingContextRoot(&inline_child)) {
    result = LayoutInlineChild(inline_child);
  } else {
    result = Layout(nullptr);
  }

  if (result->Status() == LayoutResult::kSuccess) {
    return result;
  }

  // To reduce stack usage, handle non-successful results in a separate
  // function.
  return HandleNonsuccessfulLayoutResult(result);
}

NOINLINE const LayoutResult*
BlockLayoutAlgorithm::HandleNonsuccessfulLayoutResult(
    const LayoutResult* result) {
  DCHECK_NE(result->Status(), LayoutResult::kSuccess);
  switch (result->Status()) {
    case LayoutResult::kNeedsEarlierBreak: {
      // If we found a good break somewhere inside this block, re-layout and
      // break at that location.
      DCHECK(result->GetEarlyBreak());
      return RelayoutAndBreakEarlier<BlockLayoutAlgorithm>(
          *result->GetEarlyBreak());
    }
    case LayoutResult::kNeedsLineClampRelayout:
      if (!line_clamp_data_.data.IsMeasureUntilBfcOffset()) {
        DCHECK(line_clamp_data_.data.IsClampByLines());
        DCHECK_EQ(result->LinesUntilClamp(), 0);
        return RelayoutIgnoringLineClamp();
      }
      if (GetConstraintSpace().IsNewFormattingContext()) {
        if (result->LineClampAfterLayoutObject()) {
          return RelayoutClampingAfterLayoutObject(
              result->LineClampAfterLayoutObject());
        }
        int lines_to_relayout;
        if (!line_clamp_data_.data.IsClampByLines()) {
          lines_to_relayout = result->LinesUntilClamp();
        } else {
          // If we're clamping by both lines and height, result->LinesUntilClamp
          // is the last line number before clamp, counting down from
          // Style().LineClamp().
          DCHECK_GT(Style().LineClamp(), 0);
          lines_to_relayout = Style().LineClamp() - result->LinesUntilClamp();
        }
        return RelayoutClampingByLines(lines_to_relayout);
      }
      // Propagate the error upwards until we reach the BFC root.
      return result;
    case LayoutResult::kDisableFragmentation:
      DCHECK(GetConstraintSpace().HasBlockFragmentation());
      return RelayoutWithoutFragmentation<BlockLayoutAlgorithm>();
    case LayoutResult::kTextBoxTrimEndDidNotApply:
      return RelayoutForTextBoxTrimEnd();
    default:
      return result;
  }
}

const LayoutResult* BlockLayoutAlgorithm::LayoutInlineChild(
    const InlineNode& node) {
  ParagraphScale paragraph_scale;
  if (RuntimeEnabledFeatures::CssFitWidthTextEnabled()) {
    const bool grow_consistent =
        Style().TextGrow().Target() == FitTextTarget::kConsistent;
    const bool shrink_consistent =
        Style().TextShrink().Target() == FitTextTarget::kConsistent;
    if (grow_consistent || shrink_consistent) {
      // Compute the paragraph scaling factor with a cloned
      // BlockLayoutAlgorithm.
      // TODO(crbug.com/417306102): This approach is an inefficient because it
      // handles all layout processes, and is a temporary hack until we can
      // implement a production-ready solution. Ideally we should introduce a
      // new phase separate from minmax and layout to skip unnecessary
      // processing.
      // Additionally, this feature is not currently intended to be used with
      // multi-column layouts.
      LayoutAlgorithmParams cloned_param(
          Node(), container_builder_.InitialFragmentGeometry(),
          GetConstraintSpace(), GetBreakToken(), early_break_,
          additional_early_breaks_);
      cloned_param.column_spanner_path = column_spanner_path_;
      cloned_param.previous_result = previous_result_;
      BlockLayoutAlgorithm cloned_algorithm(cloned_param);
      const LayoutResult* result =
          cloned_algorithm.LayoutInlineChild(node, nullptr);
      paragraph_scale = MeasurePerBlockScale(
          InlineNode(To<LayoutBlockFlow>(Node().GetLayoutBox())),
          result->GetPhysicalFragment(), ChildAvailableSize().inline_size);
      if ((paragraph_scale.scale < 1.0f && !shrink_consistent) ||
          (paragraph_scale.scale > 1.0f && !grow_consistent)) {
        paragraph_scale = ParagraphScale();
      }
    }
  }
  return LayoutInlineChild(node, &paragraph_scale);
}

NOINLINE const LayoutResult* BlockLayoutAlgorithm::LayoutInlineChild(
    const InlineNode& node,
    const ParagraphScale* paragraph_scale) {
  const TextWrapStyle wrap = node.Style().GetTextWrapStyle();
  if (wrap == TextWrapStyle::kPretty) [[unlikely]] {
    UseCounter::Count(node.GetDocument(), WebFeature::kTextWrapPretty);
    if (!node.IsScoreLineBreakDisabled()) {
      return LayoutWithOptimalInlineChildLayoutContext<kMaxLinesForOptimal>(
          node, paragraph_scale);
    }
  } else if (wrap == TextWrapStyle::kBalance) [[unlikely]] {
    UseCounter::Count(node.GetDocument(), WebFeature::kTextWrapBalance);
    if (!node.IsScoreLineBreakDisabled()) {
      return LayoutWithOptimalInlineChildLayoutContext<kMaxLinesForBalance>(
          node, paragraph_scale);
    }
  } else {
    DCHECK(ShouldWrapLineGreedy(wrap));
  }

  SimpleInlineChildLayoutContext context(node, &container_builder_);
  context.EnableMeasuringModeIfNecessary(paragraph_scale);
  return Layout(&context);
}

template <wtf_size_t capacity>
NOINLINE const LayoutResult*
BlockLayoutAlgorithm::LayoutWithOptimalInlineChildLayoutContext(
    const InlineNode& child,
    const ParagraphScale* paragraph_scale) {
  OptimalInlineChildLayoutContext<capacity> context(child, &container_builder_);
  context.EnableMeasuringModeIfNecessary(paragraph_scale);
  return Layout(&context);
}

NOINLINE const LayoutResult* BlockLayoutAlgorithm::RelayoutIgnoringLineClamp() {
  DCHECK(line_clamp_data_.data.IsClampByLines());
  return Relayout<BlockLayoutAlgorithm>(kRelayoutIgnoringLineClamp);
}

NOINLINE const LayoutResult* BlockLayoutAlgorithm::RelayoutClampingByLines(
    int lines_until_clamp) {
  DCHECK(line_clamp_data_.data.IsMeasureUntilBfcOffset());
  line_clamp_data_.data.lines_until_clamp = std::max(0, lines_until_clamp);
  return Relayout<BlockLayoutAlgorithm>(kRelayoutClampingByLines);
}

NOINLINE const LayoutResult*
BlockLayoutAlgorithm::RelayoutClampingAfterLayoutObject(
    const LayoutObject* layout_object) {
  DCHECK(line_clamp_data_.data.IsMeasureUntilBfcOffset());
  DCHECK(layout_object);
  line_clamp_data_.last_layout_object = layout_object;
  return Relayout<BlockLayoutAlgorithm>(kRelayoutClampingAfterLayoutObject);
}

NOINLINE const LayoutResult* BlockLayoutAlgorithm::RelayoutForTextBoxTrimEnd() {
  DCHECK(last_non_empty_inflow_child_);
  return Relayout<BlockLayoutAlgorithm>(kRelayoutForTextBoxTrim);
}

inline const LayoutResult* BlockLayoutAlgorithm::Layout(
    InlineChildLayoutContext* inline_child_layout_context) {
  IpidDepthLog ipid_depth_log("BlockLayoutAlgorithm::Layout");
  ipid_depth_log.FPrint("正在布局元素 {}。", ipid::GetNodeStr(Node()));

  DCHECK_EQ(!!inline_child_layout_context,
            Node().IsInlineFormattingContextRoot());
  container_builder_.SetIsInlineFormattingContext(inline_child_layout_context);

  if (inline_child_layout_context) {
    ipid_depth_log.FPrint(
        "该元素是内联格式化上下文根节点，将处理内联子元素的布局。");
  }

  // If this node has a column spanner inside, we'll force it to stay within the
  // current fragmentation flow, so that it doesn't establish a parallel flow,
  // even if it might have content that overflows into the next fragmentainer.
  // This way we'll prevent content that comes after the spanner from being laid
  // out *before* it.
  if (column_spanner_path_) {
    ipid_depth_log.FPrint(
        "检测到元素内部存在跨列元素，强制保持在当前分段流中，防止内容乱序。");
    container_builder_.SetShouldForceSameFragmentationFlow();
  }

  const auto& constraint_space = GetConstraintSpace();
  ipid_depth_log.FPrint(
      "当前的 ConstraintSpace 中，可用空间大小: {}，BFC 偏移: {}",
      constraint_space.AvailableSize(), constraint_space.GetBfcOffset());
  container_builder_.SetBfcLineOffset(
      constraint_space.GetBfcOffset().line_offset);

  if (auto adjoining_object_types =
          constraint_space.GetAdjoiningObjectTypes()) {
    DCHECK(!constraint_space.IsNewFormattingContext());
    DCHECK(!container_builder_.BfcBlockOffset());

    ipid_depth_log.FPrint(
        "检测到邻接对象（浮动或绝对定位元素），类型: {}。"
        "设置当 BFC 块偏移更新时中止布局的标志。",
        adjoining_object_types);
    // If there were preceding adjoining objects, they will be affected when the
    // BFC block-offset gets resolved or updated. We then need to roll back and
    // re-layout those objects with the new BFC block-offset, once the BFC
    // block-offset is updated.
    abort_when_bfc_block_offset_updated_ = true;

    container_builder_.SetAdjoiningObjectTypes(adjoining_object_types);
  } else if (constraint_space.HasBlockFragmentation()) {
    ipid_depth_log.FPrint(
        "当前空间支持块分段，设置当 BFC 块偏移更新时中止布局的标志。");
    // The offset from the block-start of the fragmentainer is part of the
    // constraint space, so if this offset changes, we need to abort.
    abort_when_bfc_block_offset_updated_ = true;
  }

  if (Style().HasLineClamp()) {
    ipid_depth_log.FPrint("元素设置了 line-clamp 属性，需要处理行数限制。");
    if (!line_clamp_data_.data.IsLineClampContext()) {
      LayoutUnit clamp_bfc_offset = ChildAvailableSize().block_size;
      ipid_depth_log.FPrint("子元素可用空间高度: {}", clamp_bfc_offset);
      if (clamp_bfc_offset == kIndefiniteSize) {
        ipid_depth_log.FPrint(
            "子元素可用空间高度不明确，需要计算元素的初始最小/最大块尺寸。");
        const MinMaxSizes sizes = ComputeInitialMinMaxBlockSizes(
            constraint_space, Node(), BorderPadding());
        ipid_depth_log.FPrint("初始块尺寸计算结果: {}", sizes);
        if (sizes.max_size != LayoutUnit::Max()) {
          clamp_bfc_offset =
              (sizes.max_size - BorderScrollbarPadding().block_end)
                  .ClampNegativeToZero();
          ipid_depth_log.FPrint(
              "根据最大尺寸 {} 减去下方边框和内边距 {} 计算得到 clamp BFC "
              "偏移: {}",
              sizes.max_size, BorderScrollbarPadding().block_end,
              clamp_bfc_offset);
        }
      } else {
        clamp_bfc_offset =
            (BorderScrollbarPadding().block_start + clamp_bfc_offset)
                .ClampNegativeToZero();
        ipid_depth_log.FPrint(
            "加上上方边框和内边距 {} 计算得到 clamp BFC 偏移: {}",
            BorderScrollbarPadding().block_start, clamp_bfc_offset);
      }

      if (clamp_bfc_offset != kIndefiniteSize) {
        WebFeature use_counter_feature;
        if (Style().WebkitLineClamp() != 0 ||
            Style().Continue() == EContinue::kWebkitLegacy) {
          use_counter_feature = WebFeature::kWebkitLineClampWithHeight;
        } else if (Style().LineClamp() == 0) {
          use_counter_feature = WebFeature::kLineClampAuto;
        } else {
          use_counter_feature = WebFeature::kLineClampByLinesAndHeight;
        }
        UseCounter::Count(Node().GetDocument(), use_counter_feature);
      }

      line_clamp_data_.UpdateFromStyle(Style().LineClamp(), clamp_bfc_offset);
      ipid_depth_log.FPrint("更新 line-clamp 数据，行数限制: {}，BFC 偏移: {}",
                            Style().LineClamp(), clamp_bfc_offset);
    }
  } else {
    if (Style().WebkitLineClamp() != 0) {
      ipid_depth_log.FPrint(
          "检测到使用了旧版 -webkit-line-clamp 属性（值: {}），"
          "将记录 WebFeature 统计。",
          Style().WebkitLineClamp());
      UseCounter::Count(Node().GetDocument(),
                        WebFeature::kWebkitLineClampWithoutWebkitBox);
    }

    // If we're clamping by BFC offset, we need to subtract the bottom bmp to
    // leave room for it. This doesn't apply if we're relaying out to fix the
    // offset, because that already accounts for the bmp.
    if (line_clamp_data_.data.IsMeasureUntilBfcOffset()) {
      ipid_depth_log.FPrint(
          "正在按 BFC 偏移来限制行数，需要调整下方的 margin 和 padding。");
      MarginStrut end_margin_strut = constraint_space.LineClampEndMarginStrut();
      BoxStrut self_margins = ComputeMarginsForSelf(constraint_space, Style());
      end_margin_strut.Append(self_margins.block_end,
                              /* is_quirky */ false);
      ipid_depth_log.FPrint(
          "计算结束 margin strut，祖先贡献: {}，当前元素下方 margin: {}，"
          "合并后: {}",
          ipid::GetMarginStrutString(
              constraint_space.LineClampEndMarginStrut()),
          self_margins.block_end, ipid::GetMarginStrutString(end_margin_strut));

      // `constraint_space.LineClampEndMarginStrut().Sum()` is the margin
      // contribution from our ancestor boxes, which has already been taken
      // into account for the clamp BFC offset that we have. We only need to
      // add any additional margin contribution from this box's margin.
      LayoutUnit margin_adjustment =
          end_margin_strut.Sum() -
          constraint_space.LineClampEndMarginStrut().Sum();
      LayoutUnit border_padding_adjustment = BorderScrollbarPadding().block_end;
      line_clamp_data_.data.clamp_bfc_offset -=
          border_padding_adjustment + margin_adjustment;
      ipid_depth_log.FPrint(
          "调整 clamp BFC 偏移，减去下方边框内边距 {} 和 margin 调整量 {}，"
          "调整后的 clamp BFC 偏移: {}",
          border_padding_adjustment, margin_adjustment,
          line_clamp_data_.data.clamp_bfc_offset);

      // The presence of borders and padding blocks margin propagation.
      if (!BorderScrollbarPadding().block_end) {
        ipid_depth_log.FPrint(
            "下方没有边框内边距，保存结束 margin strut 用于 margin 传播。");
        line_clamp_data_.end_margin_strut = end_margin_strut;
      }
    }
  }

  LayoutUnit content_edge = BorderScrollbarPadding().block_start;
  ipid_depth_log.FPrint("计算内容区域的起始位置，上方边框内边距: {}",
                        content_edge);

  PreviousInflowPosition previous_inflow_position = {
      LayoutUnit(), constraint_space.GetMarginStrut(),
      is_resuming_ ? LayoutUnit() : container_builder_.Padding().block_start,
      /* self_collapsing_child_had_clearance */ false};
  ipid_depth_log.FPrint(
      "初始化前一个内联流子元素位置信息："
      "逻辑块偏移: {}，margin strut: {}，块端注释空间: {}",
      previous_inflow_position.logical_block_offset,
      ipid::GetMarginStrutString(previous_inflow_position.margin_strut),
      previous_inflow_position.block_end_annotation_space);

  if (GetBreakToken()) {
    ipid_depth_log.FPrint("检测到断点令牌，表明正在从断点恢复布局。");
    if (IsBreakInside(GetBreakToken()) && !GetBreakToken()->IsForcedBreak() &&
        !GetBreakToken()->IsCausedByColumnSpanner()) {
      ipid_depth_log.FPrint(
          "断点令牌表明在容器内部发生了非强制断点，设置 margin 丢弃标志。");
      // If the block container is being resumed after an unforced break,
      // margins inside may be adjoining with the fragmentainer boundary.
      previous_inflow_position.margin_strut.discard_margins = true;
    }

    if (GetBreakToken()->MonolithicOverflow()) {
      ipid_depth_log.FPrint(
          "断点令牌表明存在单一块溢出，设置下一个子元素前有断点机会。");
      // If we have been pushed by monolithic overflow that started on a
      // previous page, we'll behave as if there's a valid breakpoint before the
      // first child here, and that it has perfect break appeal. This isn't
      // always strictly correct (the monolithic content in question may have
      // break-after:avoid, for instance), but should be a reasonable approach,
      // unless we want to make a bigger effort.
      has_break_opportunity_before_next_child_ = true;
    }
  }

  // Do not collapse margins between parent and its child if:
  //
  // A: There is border/padding between them.
  // B: This is a new formatting context
  // C: We're resuming layout from a break token. Margin struts cannot pass from
  //    one fragment to another if they are generated by the same block; they
  //    must be dealt with at the first fragment.
  //
  // In all those cases we can and must resolve the BFC block offset now.
  if (content_edge || is_resuming_ ||
      constraint_space.IsNewFormattingContext()) {
    ipid_depth_log.FPrint(
        "满足 margin 不折叠条件（content_edge: {}，is_resuming: {}，"
        "IsNewFormattingContext: {}），需要立即解析 BFC 块偏移。",
        content_edge, ipid::btos(is_resuming_),
        constraint_space.IsNewFormattingContext());
    bool discard_subsequent_margins =
        previous_inflow_position.margin_strut.discard_margins && !content_edge;
    if (!ResolveBfcBlockOffset(&previous_inflow_position)) {
      ipid_depth_log.FPrint("BFC 块偏移解析失败，需要中止布局等待再次尝试。");
      // There should be no preceding content that depends on the BFC block
      // offset of a new formatting context block, and likewise when resuming
      // from a break token.
      DCHECK(!constraint_space.IsNewFormattingContext());
      DCHECK(!is_resuming_);
      return container_builder_.Abort(LayoutResult::kBfcBlockOffsetResolved);
    }
    ipid_depth_log.FPrint("BFC 块偏移解析成功，将子元素位置移动到内容区域: {}",
                          content_edge);
    // Move to the content edge. This is where the first child should be placed.
    previous_inflow_position.logical_block_offset = content_edge;

    // If we resolved the BFC block offset now, the margin strut has been
    // reset. If margins are to be discarded, and this box would otherwise have
    // adjoining margins between its own margin and those subsequent content,
    // we need to make sure subsequent content discard theirs.
    if (discard_subsequent_margins) {
      ipid_depth_log.FPrint(
          "由于需要丢弃后续 margin，设置后续内容的 margin 丢弃标志。");
      previous_inflow_position.margin_strut.discard_margins = true;
    }
  }

#if DCHECK_IS_ON()
  // If this is a new formatting context, we should definitely be at the origin
  // here. If we're resuming from a break token (for a block that doesn't
  // establish a new formatting context), that may not be the case,
  // though. There may e.g. be clearance involved, or inline-start margins.
  if (constraint_space.IsNewFormattingContext()) {
    DCHECK_EQ(*container_builder_.BfcBlockOffset(), LayoutUnit());
  }
  // If this is a new formatting context, or if we're resuming from a break
  // token, no margin strut must be lingering around at this point.
  if (constraint_space.IsNewFormattingContext() || is_resuming_) {
    DCHECK(constraint_space.GetMarginStrut().IsEmpty());
  }

  if (!container_builder_.BfcBlockOffset()) {
    // New formatting-contexts, and when we have a self-collapsing child
    // affected by clearance must already have their BFC block-offset resolved.
    DCHECK(!previous_inflow_position.self_collapsing_child_had_clearance);
    DCHECK(!constraint_space.IsNewFormattingContext());
  }
#endif

  // Clamping at the start of a line-clamp container.
  // This can only happen when clamping by a height (e.g. line-clamp: auto;
  // max-height: 0).
  if (constraint_space.IsNewFormattingContext() &&
      line_clamp_data_.IsPastClampPoint()) {
    ipid_depth_log.FPrint(
        "新格式化上下文且已超过 line-clamp 点，保存当前位置信息。");
    line_clamp_data_.previous_inflow_position_when_clamped =
        previous_inflow_position;
  }

  // If this node is a quirky container, (we are in quirks mode and either a
  // table cell or body), we set our margin strut to a mode where it only
  // considers non-quirky margins. E.g.
  // <body>
  //   <p></p>
  //   <div style="margin-top: 10px"></div>
  //   <h1>Hello</h1>
  // </body>
  // In the above example <p>'s & <h1>'s margins are ignored as they are
  // quirky, and we only consider <div>'s 10px margin.
  if (node_.IsQuirkyContainer()) {
    ipid_depth_log.FPrint(
        "当前节点是怪异模式容器（table cell 或 body），"
        "设置 margin strut 仅考虑非怪异 margin。");
    previous_inflow_position.margin_strut.is_quirky_container_start = true;
  }

  // Try to reuse line box fragments from cached fragments if possible.
  // When possible, this adds fragments to |container_builder_| and update
  // |previous_inflow_position| and |BreakToken()|.
  const InlineBreakToken* previous_inline_break_token = nullptr;

  BlockChildIterator child_iterator(Node().FirstChild(), GetBreakToken());
  ipid_depth_log.FPrint("初始化子元素迭代器，第一个子元素: {}，断点令牌: {}",
                        ipid::GetNodeStr(Node().FirstChild()),
                        ipid::btos(GetBreakToken()));

  // If this layout is blocked by a display-lock, then we pretend this node has
  // no children and that there are no break tokens. Due to this, we skip layout
  // on these children.
  if (Node().ChildLayoutBlockedByDisplayLock()) {
    ipid_depth_log.FPrint("子元素布局被 display-lock 阻塞，忽略所有子元素。");
    child_iterator = BlockChildIterator(BlockNode(nullptr), nullptr);
  }

  BlockNode placeholder_child(nullptr);
  BlockChildIterator::Entry entry;
  ipid_depth_log.FPrint("开始遍历子元素列表进行布局。");
  for (entry = child_iterator.NextChild(); LayoutInputNode child = entry.node;
       entry = child_iterator.NextChild(previous_inline_break_token)) {
    const BreakToken* child_break_token = entry.token;
    ipid_depth_log.FPrint("正在处理子元素: {}", ipid::GetNodeStr(child));

    if (child.IsOutOfFlowPositioned()) {
      ipid_depth_log.FPrint(
          "子元素 {} 是绝对定位元素，调用 HandleOutOfFlowPositioned 处理。",
          ipid::GetNodeStr(child));
      // Out-of-flow fragmentation is a special step that takes place after
      // regular layout, so we should never resume anything here. However, we
      // may have break-before tokens, when a column spanner is directly
      // followed by an OOF.
      DCHECK(!child_break_token ||
             (child_break_token->IsBlockType() &&
              To<BlockBreakToken>(child_break_token)->IsBreakBefore()));
      HandleOutOfFlowPositioned(previous_inflow_position, To<BlockNode>(child));
    } else if (child.IsFloating()) {
      ipid_depth_log.FPrint("子元素 {} 是浮动元素，调用 HandleFloat 处理。",
                            ipid::GetNodeStr(child));
      HandleFloat(previous_inflow_position, To<BlockNode>(child),
                  To<BlockBreakToken>(child_break_token));
    } else if (child.IsListMarker() && !child.ListMarkerOccupiesWholeLine()) {
      ipid_depth_log.FPrint("子元素 {} 是不占据整行的列表标记，忽略处理。",
                            ipid::GetNodeStr(child));
      // Ignore outside list markers because they are already set to
      // |container_builder_.UnpositionedListMarker| in the constructor, unless
      // |ListMarkerOccupiesWholeLine|, which is handled like a regular child.
    } else if (child.IsColumnSpanAll() && constraint_space.IsInColumnBfc() &&
               constraint_space.HasBlockFragmentation()) {
      ipid_depth_log.FPrint("子元素 {} 是跨列元素，处理列分段逻辑。",
                            ipid::GetNodeStr(child));
      // The child is a column spanner. If we have no breaks inside (in parallel
      // flows), we now need to finish this fragmentainer, then abort and let
      // the column layout algorithm handle the spanner as a child. The
      // HasBlockFragmentation() check above may seem redundant, but this is
      // important if we're overflowing a clipped container. In such cases, we
      // won't treat the spanner as one, since we shouldn't insert any breaks in
      // that mode.
      DCHECK(!container_builder_.DidBreakSelf());
      DCHECK(!container_builder_.FoundColumnSpanner());
      DCHECK(!IsBreakInside(To<BlockBreakToken>(child_break_token)));

      if (constraint_space.IsPastBreak() ||
          container_builder_.HasInsertedChildBreak()) {
        ipid_depth_log.FPrint(
            "检测到内部已有断点，需要先完成跨列元素之前的内容。");
        // Something broke inside (typically in a parallel flow, or we wouldn't
        // be here). Before we can handle the spanner, we need to finish what
        // comes before it.
        container_builder_.AddBreakBeforeChild(child, kBreakAppealPerfect,
                                               /* is_forced_break */ true);

        // We're not ready to go back and lay out the spanner yet (see above),
        // so we don't set a spanner path, but since we did find a spanner, make
        // a note of it. This will make sure that we resolve our BFC block-
        // offset, so that we don't incorrectly appear to be self-collapsing.
        ipid_depth_log.FPrint("标记找到跨列元素，确保 BFC 块偏移被解析。");
        container_builder_.SetHasColumnSpanner(true);
        break;
      }

      // Establish a column spanner path. The innermost node will be the spanner
      // itself, wrapped inside the container handled by this layout algorithm.
      ipid_depth_log.FPrint("建立跨列元素路径，设置父容器为空的标记。");
      const auto* child_spanner_path =
          MakeGarbageCollected<ColumnSpannerPath>(To<BlockNode>(child));
      const auto* container_spanner_path =
          MakeGarbageCollected<ColumnSpannerPath>(Node(), child_spanner_path);
      container_builder_.SetColumnSpannerPath(container_spanner_path);

      // In order to properly collapse column spanner margins, we need to know
      // if the column spanner's parent was empty, for example, in the case that
      // the only child content of the parent since the last spanner is an OOF
      // that will get positioned outside the multicol.
      bool is_empty_parent =
          container_builder_.Children().empty() && is_resuming_;
      ipid_depth_log.FPrint(
          "设置跨列元素父容器空标记: {}（子元素数: {}，恢复标志: {}）",
          is_empty_parent, container_builder_.Children().size(),
          ipid::btos(is_resuming_));
      container_builder_.SetIsEmptySpannerParent(is_empty_parent);
      // After the spanner(s), we are going to resume inside this block. If
      // there's a subsequent sibling that's not a spanner, we're resume right
      // in front of that one. Otherwise we'll just resume after all the
      // children.
      for (entry = child_iterator.NextChild();
           LayoutInputNode sibling = entry.node;
           entry = child_iterator.NextChild()) {
        DCHECK(!entry.token);
        if (sibling.IsColumnSpanAll())
          continue;
        container_builder_.AddBreakBeforeChild(sibling, kBreakAppealPerfect,
                                               /* is_forced_break */ true);
        break;
      }
      break;
    } else if (child.IsTextControlPlaceholder()) {
      ipid_depth_log.FPrint("子元素 {} 是文本控件占位符，暂存以后处理。",
                            ipid::GetNodeStr(child));
      placeholder_child = To<BlockNode>(child);
    } else {
      // If this is the child we had previously determined to break before, do
      // so now and finish layout.
      if (early_break_ && IsEarlyBreakTarget(*early_break_, container_builder_,
                                             child)) [[unlikely]] {
        ipid_depth_log.FPrint("子元素 {} 是预先确定的断点目标，在此处断开。",
                              ipid::GetNodeStr(child));
        if (!ResolveBfcBlockOffset(&previous_inflow_position)) {
          ipid_depth_log.FPrint(
              "断点位置恰好是 BFC 块偏移解析点，中止布局等待重新布局。");
          // However, the predetermined breakpoint may be exactly where the BFC
          // block-offset gets resolved. If that hasn't yet happened, we need to
          // do that first and re-layout at the right BFC block-offset, and THEN
          // break.
          return container_builder_.Abort(
              LayoutResult::kBfcBlockOffsetResolved);
        }
        container_builder_.AddBreakBeforeChild(child, kBreakAppealPerfect,
                                               /* is_forced_break */ false);
        ConsumeRemainingFragmentainerSpace(&previous_inflow_position);
        ipid_depth_log.FPrint(
            "在子元素 {} 前添加断点，消耗剩余分段空间，结束布局。",
            ipid::GetNodeStr(child));
        break;
      }

      LayoutResult::EStatus status;
      if (child.CreatesNewFormattingContext()) {
        ipid_depth_log.FPrint(
            "子元素 {} 建立新格式化上下文，调用 HandleNewFormattingContext "
            "处理。",
            ipid::GetNodeStr(child));
        status = HandleNewFormattingContext(
            child, To<BlockBreakToken>(child_break_token),
            &previous_inflow_position);
        previous_inline_break_token = nullptr;
      } else {
        ipid_depth_log.FPrint(
            "子元素 {} 不建立新格式化上下文，调用 HandleInflow 处理。",
            ipid::GetNodeStr(child));
        status = HandleInflow(
            child, child_break_token, &previous_inflow_position,
            inline_child_layout_context, &previous_inline_break_token);
      }

      if (status != LayoutResult::kSuccess) {
        ipid_depth_log.FPrint("子元素 {} 的布局状态为 {}，中止布局。",
                              ipid::GetNodeStr(child),
                              ipid::GetLayoutResultStatusString(status));
        // We need to abort the layout. No fragment will be generated.
        return container_builder_.Abort(status);
      }
      if (constraint_space.HasBlockFragmentation()) {
        // A child break in a parallel flow doesn't affect whether we should
        // break here or not.
        if (container_builder_.HasInflowChildBreakInside()) {
          ipid_depth_log.FPrint(
              "检测到子元素内部发生了断点（同一流），结束布局。");
          // But if the break happened in the same flow, we'll now just finish
          // layout of the fragment. No more siblings should be processed.
          break;
        }
      }
    }
  }
  ipid_depth_log.FPrint("子元素遍历完成。");

#if DCHECK_IS_ON()
  // Assert that we have made actual progress. Breaking before we're done with
  // all parallel flows from incoming break tokens means that we'll never get
  // the opportunity to handle them again. We don't repropagate unhandled
  // incoming break tokens, and there should be no need to.
  //
  // However, a column spanner forces all content preceding it to stay in the
  // same flow, so we can (and must) skip the check. Even if IsAtBlockEnd() /
  // IsInParallelBlockFlow() is true in such cases, it doesn't mean that a
  // parallel flow is established.
  if (!container_builder_.FoundColumnSpanner() &&
      !container_builder_.ShouldForceSameFragmentationFlow()) {
    if (auto* inline_token = DynamicTo<InlineBreakToken>(entry.token)) {
      DCHECK(!inline_token->IsInParallelBlockFlow());
    } else if (auto* block_token = DynamicTo<BlockBreakToken>(entry.token)) {
      DCHECK(!block_token->IsAtBlockEnd());
    }
  }
#endif

  if (placeholder_child) {
    PlaceholderLayoutResult offset_and_status = HandleTextControlPlaceholder(
        placeholder_child, previous_inflow_position);
    if (offset_and_status.status != LayoutResult::kSuccess) {
      return container_builder_.Abort(offset_and_status.status);
    }
    previous_inflow_position.logical_block_offset =
        offset_and_status.logical_block_offset;
  }

  if (!child_iterator.NextChild(previous_inline_break_token).node) {
    ipid_depth_log.FPrint("所有子元素都已遍历完成，标记为已查看所有子元素。");
    // We've gone through all the children. This doesn't necessarily mean that
    // we're done fragmenting, as there may be parallel flows [1] (visible
    // overflow) still needing more space than what the current fragmentainer
    // can provide. It does mean, though, that, for any future fragmentainers,
    // we'll just be looking at the break tokens, if any, and *not* start laying
    // out any nodes from scratch, since we have started/finished all the
    // children, or at least created break tokens for them.
    //
    // [1] https://drafts.csswg.org/css-break/#parallel-flows
    container_builder_.SetHasSeenAllChildren();
  }

  // The intrinsic block size is not allowed to be less than the content edge
  // offset, as that could give us a negative content box size.
  intrinsic_block_size_ = content_edge;
  ipid_depth_log.FPrint(
      "设置内在块大小为内容区域起始位置: {}，防止负数内容盒。", content_edge);

  // To save space of the stack when we recurse into children, the rest of this
  // function is continued within |FinishLayout|. However it should be read as
  // one function.
  ipid_depth_log.FPrint("布局主要部分完成，调用 FinishLayout 完成剩余工作。");
  return FinishLayout(&previous_inflow_position, inline_child_layout_context);
}

const LayoutResult* BlockLayoutAlgorithm::FinishLayout(
    PreviousInflowPosition* previous_inflow_position,
    InlineChildLayoutContext* inline_child_layout_context) {
  IpidDepthLog ipid_depth_log("BlockLayoutAlgorithm::FinishLayout");
  ipid_depth_log.FPrint("正在完成元素 {} 的布局的收尾工作。",
                        ipid::GetNodeStr(Node()));

  const auto& constraint_space = GetConstraintSpace();
  ipid_depth_log.FPrint(
      "检查是否为新的格式化上下文: {}，line-clamp 数据状态: {}",
      ipid::btos(constraint_space.IsNewFormattingContext()),
      ipid::btos(line_clamp_data_.ShouldRelayoutWithNoForcedTruncate()));
  if (constraint_space.IsNewFormattingContext() &&
      line_clamp_data_.ShouldRelayoutWithNoForcedTruncate()) [[unlikely]] {
    // Truncation of the last line was forced, but there are no lines after the
    // truncated line. Rerun layout without forcing truncation. This is only
    // done if line-clamp was specified on the element as the element containing
    // the node may have subsequent lines. If there aren't, the containing
    // element will relayout.
    ipid_depth_log.FPrint(
        "Line-clamp "
        "强制截断了最后一行，但截断行后没有更多行，需要重新布局而不强制截断。");
    return container_builder_.Abort(LayoutResult::kNeedsLineClampRelayout);
  }

  ipid_depth_log.FPrint(
      "检查 text-box-trim-end 状态: {}，有最后非空内流子元素: {}，未进行 "
      "line-clamp: {}",
      ipid::btos(container_builder_.ShouldTextBoxTrimEnd()),
      ipid::GetNodeStr(last_non_empty_inflow_child_),
      ipid::btos(
          !line_clamp_data_.previous_inflow_position_when_clamped.has_value()));
  if (container_builder_.ShouldTextBoxTrimEnd() &&
      last_non_empty_inflow_child_ &&
      !line_clamp_data_.previous_inflow_position_when_clamped.has_value())
      [[unlikely]] {
    // The `text-box-trim: trim-end` should apply to the last inflow child, but
    // which child this was isn't always something we can tell up-front, e.g. if
    // the last formatted line is inside a block-in-inline, and we moved past it
    // with no trimming.
    // We ignore this if we have line-clamped, because the trim-end would have
    // applied to the last line before clamp regardless.
    ipid_depth_log.FPrint(
        "text-box-trim: trim-end "
        "应该应用于最后的内流子元素，但无法确定哪个是最后的子元素，需要中止布局"
        "。");
    return container_builder_.Abort(LayoutResult::kTextBoxTrimEndDidNotApply);
  }

  // With CSSLineClamp enabled, if we line-clamped inside this box, its size
  // must be set exactly as if there were no layout boxes after the clamp point.
  // We therefore use the previous inflow position that we saved at the clamp
  // point.
  if (RuntimeEnabledFeatures::CSSLineClampEnabled() &&
      line_clamp_data_.previous_inflow_position_when_clamped.has_value())
      [[unlikely]] {
    ipid_depth_log.FPrint(
        "CSS line-clamp 已启用且在此盒内进行了 line-clamp，使用保存的 clamp "
        "点的上一个内流位置。");
    previous_inflow_position =
        &*line_clamp_data_.previous_inflow_position_when_clamped;
  }

  LogicalSize border_box_size = container_builder_.InitialBorderBoxSize();
  MarginStrut end_margin_strut = previous_inflow_position->margin_strut;
  ipid_depth_log.FPrint(
      "初始边框盒大小: {}，从上一个内流位置获取的结束 margin strut: {}",
      ipid::GetLogicalSizeString(border_box_size),
      ipid::GetMarginStrutString(end_margin_strut));

  // Add line height for empty content editable or button with empty label, e.g.
  // <div contenteditable></div>, <input type="button" value="">
  if (container_builder_.HasSeenAllChildren() &&
      HasLineEvenIfEmpty(Node().GetLayoutBox())) {
    LayoutUnit empty_line_size = Node().EmptyLineBlockSize(GetBreakToken());
    ipid_depth_log.FPrint(
        "元素已看到所有子元素且即使为空也应有行高，空行大小: "
        "{}，当前内在块大小: {}",
        empty_line_size, intrinsic_block_size_);
    intrinsic_block_size_ =
        std::max(intrinsic_block_size_,
                 BorderScrollbarPadding().block_start + empty_line_size);
    ipid_depth_log.FPrint("更新后的内在块大小: {}", intrinsic_block_size_);
    if (container_builder_.IsInitialColumnBalancingPass()) {
      ipid_depth_log.FPrint("当前是初始列平衡阶段，传播最高不可分割块大小: {}",
                            intrinsic_block_size_);
      container_builder_.PropagateTallestUnbreakableBlockSize(
          intrinsic_block_size_);
    }
    // Test [1][2] require baseline offset for empty editable.
    // [1] css3/flexbox/baseline-for-empty-line.html
    // [2] inline-block/contenteditable-baseline.html
    const LayoutBlock* const layout_block =
        To<LayoutBlock>(Node().GetLayoutBox());
    if (auto baseline_offset = layout_block->BaselineForEmptyLine()) {
      ipid_depth_log.FPrint("为空的可编辑元素设置基线偏移: {}",
                            *baseline_offset);
      container_builder_.SetBaselines(*baseline_offset);
    }
  }

  // Collapse annotation overflow and padding.
  // logical_block_offset already contains block-end annotation overflow.
  // However, if the container has non-zero block-end padding, the annotation
  // can extend on the padding. So we decrease logical_block_offset by
  // shareable part of the annotation overflow and the padding.
  if (previous_inflow_position->block_end_annotation_space < LayoutUnit()) {
    const LayoutUnit annotation_overflow =
        -previous_inflow_position->block_end_annotation_space;
    LayoutUnit padding_block_end = container_builder_.Padding().block_end;
    LayoutUnit reduction = std::min(padding_block_end, annotation_overflow);
    ipid_depth_log.FPrint(
        "存在注解溢出: {}，容器下方 padding: {}，减少逻辑块偏移: {}",
        annotation_overflow, padding_block_end, reduction);
    previous_inflow_position->logical_block_offset -= reduction;
  }

  // If line clamping occurred, and we're using the legacy behavior, the
  // intrinsic block-size comes from the intrinsic block-size at the time of the
  // clamp, without taking margins, clearance, etc. into account.
  if (!RuntimeEnabledFeatures::CSSLineClampEnabled() &&
      line_clamp_data_.previous_inflow_position_when_clamped) {
    DCHECK(container_builder_.BfcBlockOffset());
    LayoutUnit clamped_size =
        line_clamp_data_.previous_inflow_position_when_clamped
            ->logical_block_offset +
        BorderScrollbarPadding().block_end;
    ipid_depth_log.FPrint(
        "使用传统行为，发生了 line clamping，内在块大小来自 clamp 时间点: {}",
        clamped_size);
    intrinsic_block_size_ = clamped_size;
    end_margin_strut = MarginStrut();
  } else if (BorderScrollbarPadding().block_end ||
             previous_inflow_position->self_collapsing_child_had_clearance ||
             constraint_space.IsNewFormattingContext()) {
    // The end margin strut of an in-flow fragment contributes to the size of
    // the current fragment if:
    //  - There is block-end border/scrollbar/padding.
    //  - There was a self-collapsing child affected by clearance.
    //  - We are a new formatting context.
    // Additionally this fragment produces no end margin strut.
    ipid_depth_log.FPrint(
        "结束 margin strut 将贡献给当前片段大小。原因: 下方 border/padding: "
        "{}，有受 clearance 影响的自折叠子元素: {}，是新格式化上下文: {}",
        BorderScrollbarPadding().block_end,
        ipid::btos(
            previous_inflow_position->self_collapsing_child_had_clearance),
        ipid::btos(constraint_space.IsNewFormattingContext()));

    // If the current layout is a new formatting context, we need to encapsulate
    // all of our floats, except for those that were hidden because of
    // line-clamp.
    if (constraint_space.IsNewFormattingContext()) {
      LayoutUnit clearance =
          GetExclusionSpace().NonHiddenClearanceOffsetIncludingInitialLetter();
      ipid_depth_log.FPrint(
          "新格式化上下文需要封装所有浮动元素，获取 clearance 偏移: {}",
          clearance);
#ifdef DCHECK_ALWAYS_ON
      if (!RuntimeEnabledFeatures::CSSLineClampEnabled() ||
          !line_clamp_data_.previous_inflow_position_when_clamped) {
        DCHECK_EQ(clearance,
                  GetExclusionSpace().ClearanceOffsetIncludingInitialLetter(
                      EClear::kBoth));
      }
#endif
      LayoutUnit old_intrinsic_size = intrinsic_block_size_;
      intrinsic_block_size_ = std::max(intrinsic_block_size_, clearance);
      if (intrinsic_block_size_ != old_intrinsic_size) {
        ipid_depth_log.FPrint("由于 clearance，内在块大小从 {} 更新为 {}",
                              old_intrinsic_size, intrinsic_block_size_);
      }
    }

    if (!container_builder_.BfcBlockOffset()) {
      // If we have collapsed through the block start and all children (if any),
      // now is the time to determine the BFC block offset, because finally we
      // have found something solid to hang on to (like clearance or a bottom
      // border, for instance). If we're a new formatting context, though, we
      // shouldn't be here, because then the offset should already have been
      // determined.
      ipid_depth_log.FPrint(
          "容器的 BFC 块偏移仍未知，需要解析 BFC "
          "块偏移。当前不是新格式化上下文: {}",
          ipid::btos(!constraint_space.IsNewFormattingContext()));
      DCHECK(!constraint_space.IsNewFormattingContext());
      if (!ResolveBfcBlockOffset(previous_inflow_position)) {
        ipid_depth_log.FPrint("ResolveBfcBlockOffset 返回 false，中止布局。");
        return container_builder_.Abort(LayoutResult::kBfcBlockOffsetResolved);
      }
      DCHECK(container_builder_.BfcBlockOffset());
      ipid_depth_log.FPrint("成功解析 BFC 块偏移: {}",
                            *container_builder_.BfcBlockOffset());
    } else {
      // If we are a quirky container, we ignore any quirky margins and just
      // consider normal margins to extend our size.  Other UAs perform this
      // calculation differently, e.g. by just ignoring the *last* quirky
      // margin.
      LayoutUnit margin_strut_sum = node_.IsQuirkyContainer()
                                        ? end_margin_strut.QuirkyContainerSum()
                                        : end_margin_strut.Sum();
      ipid_depth_log.FPrint(
          "计算 margin strut 总和。是 quirky 容器: {}，margin strut: "
          "{}，margin strut 总和: {}",
          ipid::btos(node_.IsQuirkyContainer()),
          ipid::GetMarginStrutString(end_margin_strut), margin_strut_sum);

      if (constraint_space.HasKnownFragmentainerBlockSize()) {
        LayoutUnit new_margin_strut_sum = AdjustedMarginAfterFinalChildFragment(
            container_builder_, previous_inflow_position->logical_block_offset,
            margin_strut_sum);
        if (new_margin_strut_sum != margin_strut_sum) {
          ipid_depth_log.FPrint(
              "在已知分片器块大小的情况下，调整后的 margin strut 总和从 {} "
              "变为 {}，设置被分片线截断标志。",
              margin_strut_sum, new_margin_strut_sum);
          container_builder_.SetIsTruncatedByFragmentationLine();
          margin_strut_sum = new_margin_strut_sum;
        }
      }

      // The trailing margin strut will be part of our intrinsic block size, but
      // only if there is something that separates the end margin strut from the
      // input margin strut (typically child content, block start
      // border/padding, or this being a new BFC). If the margin strut from a
      // previous sibling or ancestor managed to collapse through all our
      // children (if any at all, that is), it means that the resulting end
      // margin strut actually pushes us down, and it should obviously not be
      // doubly accounted for as our block size.
      LayoutUnit candidate_size =
          previous_inflow_position->logical_block_offset + margin_strut_sum;
      LayoutUnit old_intrinsic_size = intrinsic_block_size_;
      intrinsic_block_size_ = std::max(intrinsic_block_size_, candidate_size);
      ipid_depth_log.FPrint(
          "计算内在块大小: 上一个内流位置的逻辑块偏移 {} + margin strut 总和 "
          "{} = {}，原内在大小: {}，更新后: {}",
          previous_inflow_position->logical_block_offset, margin_strut_sum,
          candidate_size, old_intrinsic_size, intrinsic_block_size_);
    }

    if (!ShouldIncludeBlockEndBorderPadding(container_builder_)) {
      // The block-end edge isn't in this fragment. We either haven't got there
      // yet, or we're past it (and are overflowing). So don't add trailing
      // border/padding.
      ipid_depth_log.FPrint(
          "不应包含块结尾的 border/padding，清除容器构建器的块结尾 "
          "border/padding。");
      container_builder_.ClearBorderScrollbarPaddingBlockEnd();
    }
    LayoutUnit added_padding = BorderScrollbarPadding().block_end;
    ipid_depth_log.FPrint(
        "在内在块大小上添加块结尾 border/padding: {}，更新前: {}",
        added_padding, intrinsic_block_size_);
    intrinsic_block_size_ += added_padding;
    ipid_depth_log.FPrint("更新后的内在块大小: {}，重置结束 margin strut。",
                          intrinsic_block_size_);
    end_margin_strut = MarginStrut();
  } else {
    // Update our intrinsic block size to be just past the block-end border edge
    // of the last in-flow child. The pending margin is to be propagated to our
    // container, so ignore it.
    LayoutUnit old_intrinsic_size = intrinsic_block_size_;
    intrinsic_block_size_ = std::max(
        intrinsic_block_size_, previous_inflow_position->logical_block_offset);
    ipid_depth_log.FPrint(
        "不符合特殊条件，将内在块大小更新为最后内流子元素的块结尾边框边缘后的位"
        "置。原大小: {}，上一个内流位置: {}，更新后: {}",
        old_intrinsic_size, previous_inflow_position->logical_block_offset,
        intrinsic_block_size_);
  }

  LayoutUnit unconstrained_intrinsic_block_size = intrinsic_block_size_;
  std::optional<LayoutUnit> quirky_body_margin =
      CalculateQuirkyBodyMarginBlockSum(end_margin_strut);
  ipid_depth_log.FPrint(
      "保存无约束内在块大小: {}，计算 quirky body margin: {} (若为 -999999 "
      "表示无值)",
      unconstrained_intrinsic_block_size,
      quirky_body_margin.value_or(LayoutUnit(-999999)));
  intrinsic_block_size_ = ClampIntrinsicBlockSize(
      constraint_space, Node(), GetBreakToken(), BorderScrollbarPadding(),
      intrinsic_block_size_, quirky_body_margin);
  ipid_depth_log.FPrint("经过 ClampIntrinsicBlockSize 约束后的内在块大小: {}",
                        intrinsic_block_size_);

  // In order to calculate the block-size for the fragment, we need to compare
  // the combined intrinsic block-size of all fragments to e.g. specified
  // block-size. We'll skip this part if this is a fragmentainer.
  // Fragmentainers never have a specified block-size anyway, but, more
  // importantly, adding consumed block-size, and then subtracting it again
  // later (when setting the final fragment size) would produce incorrect
  // results if the sum becomes "infinity", i.e. LayoutUnit::Max(). Skipping
  // this will allow the total block-size of all the fragmentainers to become
  // greater than LayoutUnit::Max(). This is important for column balancing, or
  // we'd fail to finish very tall child content properly, ending up with too
  // many fragmentainers, since the fragmentainers produced would be too short
  // to fit as much as necessary. Basically: don't mess up (clamp) the measument
  // we've already done.
  LayoutUnit previously_consumed_block_size;
  if (GetBreakToken() && !container_builder_.IsFragmentainerBoxType())
      [[unlikely]] {
    previously_consumed_block_size = GetBreakToken()->ConsumedBlockSize();
    ipid_depth_log.FPrint(
        "有中断 token 且不是分片器盒类型，之前消耗的块大小: {}",
        previously_consumed_block_size);
  }

  // Recompute the block-axis size now that we know our content size.
  LayoutUnit combined_size =
      previously_consumed_block_size + intrinsic_block_size_;
  ipid_depth_log.FPrint(
      "重新计算块轴大小。之前消耗: {}，当前内在: {}，结合后: {}，内联大小: {}",
      previously_consumed_block_size, intrinsic_block_size_, combined_size,
      border_box_size.inline_size);
  border_box_size.block_size =
      ComputeBlockSizeForFragment(constraint_space, Node(), BorderPadding(),
                                  combined_size, border_box_size.inline_size);
  ipid_depth_log.FPrint("计算得到的最终边框盒大小: {}",
                        ipid::GetLogicalSizeString(border_box_size));
  container_builder_.SetFragmentsTotalBlockSize(border_box_size.block_size);

  // If our BFC block-offset is still unknown, we check:
  //  - If we have a non-zero block-size (margins don't collapse through us).
  //  - If we have a break token. (Even if we are self-collapsing we position
  //    ourselves at the very start of the fragmentainer).
  //  - We got interrupted by a column spanner.
  if (!container_builder_.BfcBlockOffset() &&
      (border_box_size.block_size || GetBreakToken() ||
       container_builder_.FoundColumnSpanner())) {
    ipid_depth_log.FPrint(
        "BFC 块偏移仍未知且满足条件：非零块大小: {}，有中断 token: "
        "{}，发现列跨越器: {}",
        ipid::btos(border_box_size.block_size), ipid::btos(GetBreakToken()),
        ipid::btos(container_builder_.FoundColumnSpanner()));
    if (!ResolveBfcBlockOffset(previous_inflow_position)) {
      ipid_depth_log.FPrint("ResolveBfcBlockOffset 失败，中止布局。");
      return container_builder_.Abort(LayoutResult::kBfcBlockOffsetResolved);
    }
    DCHECK(container_builder_.BfcBlockOffset());
    ipid_depth_log.FPrint("成功解析 BFC 块偏移: {}",
                          *container_builder_.BfcBlockOffset());
  }

  if (container_builder_.BfcBlockOffset()) {
    // Do not collapse margins between the last in-flow child and bottom margin
    // of its parent if:
    //  - The block-size differs from the intrinsic size.
    //  - The parent has a definite initial block-size.
    const LayoutUnit initial_block_size = ComputeInitialBlockSizeForFragment(
        constraint_space, Node(), BorderPadding(), kIndefiniteSize,
        border_box_size.inline_size);
    bool should_reset_margin =
        (border_box_size.block_size != intrinsic_block_size_ ||
         initial_block_size != kIndefiniteSize);
    ipid_depth_log.FPrint(
        "已知 BFC 块偏移。计算的初始块大小: {}，最终边框盒块大小: "
        "{}，内在块大小: {}，是否重置 margin: {}",
        initial_block_size, border_box_size.block_size, intrinsic_block_size_,
        ipid::btos(should_reset_margin));
    if (should_reset_margin) {
      end_margin_strut = MarginStrut();
    }
  }

  // List markers should have been positioned if we had line boxes, or boxes
  // that have line boxes. If there were no line boxes, position without line
  // boxes.
  auto has_unpositioned_marker = container_builder_.GetUnpositionedListMarker();
  bool should_place_marker = ShouldPlaceUnpositionedListMarker();
  bool has_inflow_break = container_builder_.HasInflowChildBreakInside();
  ipid_depth_log.FPrint(
      "检查列表标记状态: 有未定位标记: {}，应该放置: {}，有内流中断: {}",
      ipid::btos(!!has_unpositioned_marker), ipid::btos(should_place_marker),
      ipid::btos(has_inflow_break));
  if (has_unpositioned_marker && should_place_marker &&
      // If the list-item is block-fragmented, leave it unpositioned and expect
      // following fragments have a line box.
      !has_inflow_break) {
    ipid_depth_log.FPrint("尝试在没有行框的情况下定位列表标记。");
    if (!PositionListMarkerWithoutLineBoxes(previous_inflow_position)) {
      ipid_depth_log.FPrint(
          "PositionListMarkerWithoutLineBoxes 失败，中止布局。");
      return container_builder_.Abort(LayoutResult::kBfcBlockOffsetResolved);
    }
  }

  container_builder_.SetEndMarginStrut(end_margin_strut);
  container_builder_.SetIntrinsicBlockSize(intrinsic_block_size_);
  ipid_depth_log.FPrint(
      "设置容器构建器的结束 margin strut: {} 和内在块大小: {}",
      ipid::GetMarginStrutString(end_margin_strut), intrinsic_block_size_);

  if (container_builder_.BfcBlockOffset()) {
    // If we know our BFC block-offset we should have correctly placed all
    // adjoining objects, and shouldn't propagate this information to siblings.
    ipid_depth_log.FPrint("已知 BFC 块偏移，重置邻接对象类型。");
    container_builder_.ResetAdjoiningObjectTypes();
  } else {
    // If we don't know our BFC block-offset yet, we know that for
    // margin-collapsing purposes we are self-collapsing.
    ipid_depth_log.FPrint("不知道 BFC 块偏移，设置为自折叠。");
    container_builder_.SetIsSelfCollapsing();

    // If we've been forced at a particular BFC block-offset, (either from
    // clearance past adjoining floats, or a re-layout), we can safely set our
    // BFC block-offset now.
    if (constraint_space.ForcedBfcBlockOffset()) {
      LayoutUnit forced_offset = *constraint_space.ForcedBfcBlockOffset();
      ipid_depth_log.FPrint(
          "约束空间中有强制的 BFC 块偏移: {}，设置给容器构建器。",
          forced_offset);
      container_builder_.SetBfcBlockOffset(forced_offset);

      // Also make sure that this is treated as a valid class C breakpoint (if
      // it is one).
      if (constraint_space.IsPushedByFloats()) {
        ipid_depth_log.FPrint("约束空间被浮动元素推动，设置标志。");
        container_builder_.SetIsPushedByFloats();
      }
    }
  }

  if (InvolvedInBlockFragmentation(container_builder_)) [[unlikely]] {
    ipid_depth_log.FPrint("参与块分片，调用 FinalizeForFragmentation。");
    BreakStatus status = FinalizeForFragmentation();
    ipid_depth_log.FPrint("分片处理结果: {}", static_cast<int>(status));
    if (status != BreakStatus::kContinue) {
      if (status == BreakStatus::kNeedsEarlierBreak) {
        ipid_depth_log.FPrint("需要更早的中断，中止布局。");
        return container_builder_.Abort(LayoutResult::kNeedsEarlierBreak);
      }
      DCHECK_EQ(status, BreakStatus::kDisableFragmentation);
      ipid_depth_log.FPrint("禁用分片，中止布局。");
      return container_builder_.Abort(LayoutResult::kDisableFragmentation);
    }

    // Read the intrinsic block-size back, since it may have been reduced due to
    // fragmentation.
    LayoutUnit old_intrinsic_size = intrinsic_block_size_;
    intrinsic_block_size_ = container_builder_.IntrinsicBlockSize();
    if (intrinsic_block_size_ != old_intrinsic_size) {
      ipid_depth_log.FPrint("由于分片，内在块大小从 {} 变为 {}",
                            old_intrinsic_size, intrinsic_block_size_);
    }
  } else {
#if DCHECK_IS_ON()
  // If we're not participating in a fragmentation context, no block
  // fragmentation related fields should have been set.
  ipid_depth_log.FPrint("不参与分片上下文，检查分片相关字段。");
  container_builder_.CheckNoBlockFragmentation();
#endif
  }

  // At this point, perform any final table-cell adjustments needed.
  if (constraint_space.IsTableCell()) {
    ipid_depth_log.FPrint("进行表格单元格的最终调整，内在块大小: {}",
                          intrinsic_block_size_);
    FinalizeTableCellLayout(intrinsic_block_size_, &container_builder_);
  } else {
    ipid_depth_log.FPrint("对齐块内容，无约束内在块大小: {}",
                          unconstrained_intrinsic_block_size);
    AlignBlockContent(Style(), GetBreakToken(),
                      unconstrained_intrinsic_block_size, container_builder_);
  }

  ipid_depth_log.FPrint("处理脚本定位的元素和特殊后代。");
  container_builder_.HandleOofsAndSpecialDescendants();

  if (constraint_space.GetBaselineAlgorithmType() ==
      BaselineAlgorithmType::kInlineBlock) {
    ipid_depth_log.FPrint(
        "基线算法类型为 inline-block，设置使用最后一个基线作为内联基线。");
    container_builder_.SetUseLastBaselineForInlineBaseline();
  }

  // An exclusion space is confined to nodes within the same formatting context.
  if (constraint_space.IsNewFormattingContext()) {
    ipid_depth_log.FPrint("是新格式化上下文，设置空的排除空间。");
    container_builder_.SetExclusionSpace(ExclusionSpace());
  } else {
    ipid_depth_log.FPrint("非新格式化上下文，设置 line-clamp 相关信息。");
    container_builder_.SetLinesUntilClamp(
        line_clamp_data_.data.LinesUntilClamp(/*show_measured_lines*/ true));
    container_builder_.SetLineClampAfterLayoutObject(
        line_clamp_data_.last_layout_object);
  }

  if (constraint_space.UseFirstLineStyle()) {
    ipid_depth_log.FPrint("使用首行样式，设置样式变体为 FirstLine。");
    container_builder_.SetStyleVariant(StyleVariant::kFirstLine);
  }

  ipid_depth_log.FPrint("完成布局，转换为盒片段返回。");
  return container_builder_.ToBoxFragment();
}

bool BlockLayoutAlgorithm::TryReuseFragmentsFromCache(
    InlineNode inline_node,
    PreviousInflowPosition* previous_inflow_position,
    const InlineBreakToken** inline_break_token_out) {
  DCHECK(previous_result_);

  // No lines are reusable if this block uses paragraph-level line breakers such
  // as `ParagraphLineBreaker` or `ScoreLineBreaker`.
  if (!Style().ShouldWrapLineGreedy()) {
    return false;
  }

  const auto& previous_fragment =
      To<PhysicalBoxFragment>(previous_result_->GetPhysicalFragment());
  const FragmentItems* previous_items = previous_fragment.Items();
  DCHECK(previous_items);

  // Find reusable lines. Fail if no items are reusable.
  // TODO(kojii): |DirtyLinesFromNeedsLayout| is needed only once for a
  // |LayoutBlockFlow|, not for every fragment.
  FragmentItems::DirtyLinesFromNeedsLayout(*inline_node.GetLayoutBlockFlow());
  const FragmentItem* end_item =
      previous_items->EndOfReusableItems(previous_fragment);
  DCHECK(end_item);
  if (!end_item || end_item == &previous_items->front())
    return false;

  wtf_size_t max_lines = 0;
  if (std::optional<int> lines_until_clamp =
          line_clamp_data_.LinesUntilClamp()) {
    // There is an additional logic for the last clamped line. Reuse only up to
    // before that to use the same logic.
    if (*lines_until_clamp <= 1) {
      return false;
    }
    max_lines = *lines_until_clamp - 1;
  }

  const auto& children = container_builder_.Children();
  const wtf_size_t children_before = children.size();
  FragmentItemsBuilder* items_builder = container_builder_.ItemsBuilder();
  const auto& space = GetConstraintSpace();
  DCHECK_EQ(items_builder->GetWritingDirection(), space.GetWritingDirection());
  const auto result = items_builder->AddPreviousItems(
      previous_fragment, *previous_items, *end_item, &container_builder_,
      max_lines);
  if (!result.succeeded) [[unlikely]] {
    DCHECK_EQ(children.size(), children_before);
    DCHECK(!result.used_block_size);
    DCHECK(!result.inline_break_token);
    return false;
  }

  // To reach here we mustn't have any adjoining objects, and the first line
  // must have content. Resolving the BFC block-offset here should never fail.
  DCHECK(!abort_when_bfc_block_offset_updated_);
  bool success = ResolveBfcBlockOffset(previous_inflow_position);
  DCHECK(success);
  DCHECK(container_builder_.BfcBlockOffset());

  DCHECK_GT(result.line_count, 0u);
  if (max_lines) {
    DCHECK(result.line_count <= max_lines);
    DCHECK(line_clamp_data_.data.IsClampByLines());
    line_clamp_data_.data.lines_until_clamp -= result.line_count;
  } else if (line_clamp_data_.data.IsMeasureUntilBfcOffset()) {
    line_clamp_data_.data.lines_until_clamp += result.line_count;
  }

  // |AddPreviousItems| may have added more than one lines. Propagate baselines
  // from them.
  for (const auto& child : base::span(children).subspan(children_before)) {
    DCHECK(child.fragment->IsLineBox());
    PropagateBaselineFromLineBox(*child.fragment, child.offset.block_offset);
  }

  previous_inflow_position->logical_block_offset += result.used_block_size;
  *inline_break_token_out = result.inline_break_token;
  return true;
}

void BlockLayoutAlgorithm::HandleOutOfFlowPositioned(
    const PreviousInflowPosition& previous_inflow_position,
    BlockNode child) {
  if (GetConstraintSpace().HasBlockFragmentation()) {
    // Forced breaks cannot be specified directly on out-of-flow positioned
    // elements, but if the preceding block has a forced break after, we need to
    // break before it. Note that we really only need to do this if block-start
    // offset is auto (but it's harmless to do it also when it's non-auto).
    EBreakBetween break_between =
        container_builder_.JoinedBreakBetweenValue(EBreakBetween::kAuto);
    if (IsForcedBreakValue(GetConstraintSpace(), break_between)) {
      container_builder_.AddBreakBeforeChild(child, kBreakAppealPerfect,
                                             /* is_forced_break*/ true);
      return;
    }
  }

  DCHECK(child.IsOutOfFlowPositioned());
  LogicalOffset static_offset = {BorderScrollbarPadding().inline_start,
                                 previous_inflow_position.logical_block_offset};

  // We only include the margin strut in the OOF static-position if we know we
  // aren't going to be a zero-block-size fragment.
  if (container_builder_.BfcBlockOffset())
    static_offset.block_offset += previous_inflow_position.margin_strut.Sum();

  if (child.Style().IsOriginalDisplayInlineType()) {
    // The static-position of inline-level OOF-positioned nodes depends on
    // previous floats (if any).
    //
    // Due to this we need to mark this node as having adjoining objects, and
    // perform a re-layout if our position shifts.
    if (!container_builder_.BfcBlockOffset()) {
      container_builder_.AddAdjoiningObjectTypes(kAdjoiningInlineOutOfFlow);
      abort_when_bfc_block_offset_updated_ = true;
    }

    LayoutUnit origin_bfc_block_offset =
        container_builder_.BfcBlockOffset().value_or(
            GetConstraintSpace().ExpectedBfcBlockOffset()) +
        static_offset.block_offset;

    BfcOffset origin_bfc_offset = {
        GetConstraintSpace().GetBfcOffset().line_offset +
            BorderScrollbarPadding().LineLeft(Style().Direction()),
        origin_bfc_block_offset};

    static_offset.inline_offset += CalculateOutOfFlowStaticInlineLevelOffset(
        Style(), origin_bfc_offset, GetExclusionSpace(),
        ChildAvailableSize().inline_size);

    container_builder_.AddOutOfFlowChildCandidate(child, static_offset);
  } else {
    WritingDirectionMode parent_writing_direction =
        GetConstraintSpace().GetWritingDirection();
    auto inline_axis_edge = InlineStaticPositionEdge(
        child, /*justify_items_style=*/&Style(), parent_writing_direction);
    // 'align-items' doesn't apply in block layout, so don't apply it to OOF
    // items.
    auto block_axis_edge = BlockStaticPositionEdge(
        child, /*align_items_style=*/nullptr, parent_writing_direction);

    // The alignment container for block OOF elements is a zero-thickness line
    // in the inline direction. As such, we need to adjust the inline static
    // position offset for end/center alignment to ensure the OOF ends up
    // aligned correctly within its alignment container. The block offset will
    // not change.
    //
    // https://drafts.csswg.org/css-position-3/#staticpos-rect
    LayoutUnit available_inline_size = ChildAvailableSize().inline_size;
    switch (inline_axis_edge) {
      case LogicalStaticPosition::InlineEdge::kInlineCenter:
        static_offset.inline_offset += available_inline_size / 2;
        break;
      case LogicalStaticPosition::InlineEdge::kInlineEnd:
        static_offset.inline_offset += available_inline_size;
        break;
      case LogicalStaticPosition::InlineEdge::kInlineStart:
        // The static position is already correct in this case.
        break;
    }

    container_builder_.AddOutOfFlowChildCandidate(
        child, static_offset, inline_axis_edge, block_axis_edge);
  }
}

void BlockLayoutAlgorithm::HandleFloat(
    const PreviousInflowPosition& previous_inflow_position,
    BlockNode child,
    const BlockBreakToken* child_break_token) {
  // If we're resuming layout, we must always know our position in the BFC.
  DCHECK(!IsBreakInside(child_break_token) ||
         container_builder_.BfcBlockOffset());
  const auto& constraint_space = GetConstraintSpace();

  // If we don't have a BFC block-offset yet, the "expected" BFC block-offset
  // is used to optimistically place floats.
  BfcOffset origin_bfc_offset = {
      constraint_space.GetBfcOffset().line_offset +
          BorderScrollbarPadding().LineLeft(constraint_space.Direction()),
      container_builder_.BfcBlockOffset()
          ? NextBorderEdge(previous_inflow_position)
          : constraint_space.ExpectedBfcBlockOffset()};

  if (child_break_token) {
    // If there's monolithic content inside the float from a previous page
    // overflowing into this one, move past it. And subtract any such overflow
    // from the parent flow, as floats establish a parallel flow.
    origin_bfc_offset.block_offset += child_break_token->MonolithicOverflow() -
                                      GetBreakToken()->MonolithicOverflow();
  }

  if (GetConstraintSpace().HasBlockFragmentation()) {
    // Forced breaks cannot be specified directly on floats, but if the
    // preceding block has a forced break after, we need to break before this
    // float.
    EBreakBetween break_between =
        container_builder_.JoinedBreakBetweenValue(EBreakBetween::kAuto);
    if (IsForcedBreakValue(constraint_space, break_between)) {
      container_builder_.AddBreakBeforeChild(child, kBreakAppealPerfect,
                                             /* is_forced_break*/ true);
      return;
    }
  }

  UnpositionedFloat unpositioned_float(
      child, child_break_token, ChildAvailableSize(),
      PercentageSizeForChild(child), origin_bfc_offset, constraint_space,
      Style(), FragmentainerCapacityForChildren(),
      FragmentainerOffsetForChildren(), line_clamp_data_.ShouldHideForPaint());

  if (!container_builder_.BfcBlockOffset()) {
    container_builder_.AddAdjoiningObjectTypes(
        unpositioned_float.IsLineLeft(constraint_space.Direction())
            ? kAdjoiningFloatLeft
            : kAdjoiningFloatRight);
    // If we don't have a forced BFC block-offset yet, we'll optimistically
    // place floats at the "expected" BFC block-offset. If this differs from
    // our final BFC block-offset we'll need to re-layout.
    if (!constraint_space.ForcedBfcBlockOffset()) {
      abort_when_bfc_block_offset_updated_ = true;
    }
  }

  PositionedFloat positioned_float =
      PositionFloat(&unpositioned_float, &GetExclusionSpace());

  if (positioned_float.minimum_space_shortage > LayoutUnit()) {
    container_builder_.PropagateSpaceShortage(
        positioned_float.minimum_space_shortage);
  }

  if (positioned_float.break_before_token) {
    DCHECK(constraint_space.HasBlockFragmentation());
    container_builder_.AddBreakToken(positioned_float.break_before_token,
                                     /* is_in_parallel_flow */ true);
    // After breaking before the float, carry on with layout of this
    // container. The float constitutes a parallel flow, and there may be
    // siblings that could still fit in the current fragmentainer.
    return;
  }

  DCHECK_EQ(positioned_float.layout_result->Status(), LayoutResult::kSuccess);

  // TODO(mstensho): There should be a class A breakpoint between a float and
  // another float, and also between a float and an in-flow block.

  const PhysicalFragment& physical_fragment =
      positioned_float.layout_result->GetPhysicalFragment();
  LayoutUnit float_inline_size =
      LogicalFragment(constraint_space.GetWritingDirection(), physical_fragment)
          .InlineSize();

  BfcOffset bfc_offset = {constraint_space.GetBfcOffset().line_offset,
                          container_builder_.BfcBlockOffset().value_or(
                              constraint_space.ExpectedBfcBlockOffset())};

  LogicalOffset logical_offset = LogicalFromBfcOffsets(
      positioned_float.bfc_offset, bfc_offset, float_inline_size,
      container_builder_.InlineSize(), constraint_space.Direction());

  container_builder_.AddResult(*positioned_float.layout_result, logical_offset);
}

LayoutResult::EStatus BlockLayoutAlgorithm::HandleNewFormattingContext(
    LayoutInputNode child,
    const BlockBreakToken* child_break_token,
    PreviousInflowPosition* previous_inflow_position) {
  DCHECK(child);
  DCHECK(!child.IsFloating());
  DCHECK(!child.IsOutOfFlowPositioned());
  DCHECK(child.CreatesNewFormattingContext());
  DCHECK(child.IsBlock());

  const auto& constraint_space = GetConstraintSpace();
  const ComputedStyle& child_style = child.Style();
  const TextDirection direction = constraint_space.Direction();
  InflowChildData child_data =
      ComputeChildData(*previous_inflow_position, child, child_break_token,
                       /* is_new_fc */ true);

  LayoutUnit child_origin_line_offset =
      constraint_space.GetBfcOffset().line_offset +
      BorderScrollbarPadding().LineLeft(direction);

  // If the child has a block-start margin, and the BFC block offset is still
  // unresolved, and we have preceding adjoining floats, things get complicated
  // here. Depending on whether the child fits beside the floats, the margin may
  // or may not be adjoining with the current margin strut. This affects the
  // position of the preceding adjoining floats. We may have to resolve the BFC
  // block offset once with the child's margin tentatively adjoining, then
  // realize that the child isn't going to fit beside the floats at the current
  // position, and therefore re-resolve the BFC block offset with the child's
  // margin non-adjoining. This is akin to clearance.
  MarginStrut adjoining_margin_strut(previous_inflow_position->margin_strut);
  adjoining_margin_strut.Append(child_data.margins.block_start,
                                child_style.HasMarginBlockStartQuirk());
  LayoutUnit adjoining_bfc_offset_estimate =
      child_data.bfc_offset_estimate.block_offset +
      adjoining_margin_strut.Sum();
  LayoutUnit non_adjoining_bfc_offset_estimate =
      child_data.bfc_offset_estimate.block_offset +
      previous_inflow_position->margin_strut.Sum();
  LayoutUnit child_bfc_offset_estimate = adjoining_bfc_offset_estimate;
  bool bfc_offset_already_resolved = false;
  bool child_determined_bfc_offset = false;
  bool child_margin_got_separated = false;
  bool has_adjoining_floats = false;

  if (!container_builder_.BfcBlockOffset()) {
    has_adjoining_floats =
        container_builder_.GetAdjoiningObjectTypes() & kAdjoiningFloatBoth;

    // If this node, or an arbitrary ancestor had clearance past adjoining
    // floats, we consider the margin "separated". We should *never* attempt to
    // re-resolve the BFC block-offset in this case.
    bool has_clearance_past_adjoining_floats =
        constraint_space.AncestorHasClearancePastAdjoiningFloats() ||
        HasClearancePastAdjoiningFloats(
            container_builder_.GetAdjoiningObjectTypes(), child_style, Style());

    if (has_clearance_past_adjoining_floats) {
      child_bfc_offset_estimate = NextBorderEdge(*previous_inflow_position);
      child_margin_got_separated = true;
    } else if (constraint_space.ForcedBfcBlockOffset()) {
      // This is not the first time we're here. We already have a suggested BFC
      // block offset.
      bfc_offset_already_resolved = true;
      child_bfc_offset_estimate = *constraint_space.ForcedBfcBlockOffset();
      // We require that the BFC block offset be the one we'd get with margins
      // adjoining, margins separated, or if clearance was applied to either of
      // these. Anything else is a bug.
      DCHECK(child_bfc_offset_estimate == adjoining_bfc_offset_estimate ||
             child_bfc_offset_estimate == non_adjoining_bfc_offset_estimate ||
             child_bfc_offset_estimate == constraint_space.ClearanceOffset());
      // Figure out if the child margin has already got separated from the
      // margin strut or not.
      //
      // TODO(mstensho): We get false positives here, if the container was
      // cleared by floats (but the child wasn't). See
      // wpt/css/css-break/class-c-breakpoint-after-float-004.html
      child_margin_got_separated =
          child_bfc_offset_estimate != adjoining_bfc_offset_estimate;
    }

    // The BFC block offset of this container gets resolved because of this
    // child.
    child_determined_bfc_offset = true;

    // The block-start margin of the child will only affect the parent's
    // position if it is adjoining.
    if (!child_margin_got_separated) {
      SetSubtreeModifiedMarginStrutIfNeeded(
          &child_style.MarginBlockStartUsing(Style()));
    }

    if (!ResolveBfcBlockOffset(previous_inflow_position,
                               child_bfc_offset_estimate)) {
      // If we need to abort here, it means that we had preceding unpositioned
      // floats. This is only expected if we're here for the first time.
      DCHECK(!bfc_offset_already_resolved);
      return LayoutResult::kBfcBlockOffsetResolved;
    }

    // We reset the block offset here as it may have been affected by clearance.
    child_bfc_offset_estimate = ContainerBfcOffset().block_offset;
  }

  // If the child has a non-zero block-start margin, our initial estimate will
  // be that any pending floats will be flush (block-start-wise) with this
  // child, since they are affected by margin collapsing. Furthermore, this
  // child's margin may also pull parent blocks downwards. However, this is only
  // the case if the child fits beside the floats at the current block
  // offset. If it doesn't (or if it gets clearance), the child needs to be
  // pushed down. In this case, the child's margin no longer collapses with the
  // previous margin strut, so the pending floats and parent blocks need to
  // ignore this margin, which may cause them to end up at completely different
  // positions than initially estimated. In other words, we'll need another
  // layout pass if this happens.
  bool abort_if_cleared = child_data.margins.block_start != LayoutUnit() &&
                          !child_margin_got_separated &&
                          child_determined_bfc_offset;
  BfcOffset child_bfc_offset;
  BoxStrut resolved_margins;
  const LayoutResult* layout_result = LayoutNewFormattingContext(
      child, child_break_token, child_data,
      {child_origin_line_offset, child_bfc_offset_estimate}, abort_if_cleared,
      &child_bfc_offset, &resolved_margins);

  if (!layout_result) {
    DCHECK(abort_if_cleared);
    // Layout got aborted, because the child got pushed down by floats, and we
    // may have had pending floats that we tentatively positioned incorrectly
    // (since the child's margin shouldn't have affected them). Try again
    // without the child's margin. So, we need another layout pass. Figure out
    // if we can do it right away from here, or if we have to roll back and
    // reposition floats first.
    if (child_determined_bfc_offset) {
      // The BFC block offset was calculated when we got to this child, with
      // the child's margin adjoining. Since that turned out to be wrong,
      // re-resolve the BFC block offset without the child's margin.
      LayoutUnit old_offset = *container_builder_.BfcBlockOffset();
      container_builder_.ResetBfcBlockOffset();

      // Re-resolving the BFC block-offset with a different "forced" BFC
      // block-offset is only safe if an ancestor *never* had clearance past
      // adjoining floats.
      DCHECK(!constraint_space.AncestorHasClearancePastAdjoiningFloats());
      ResolveBfcBlockOffset(previous_inflow_position,
                            non_adjoining_bfc_offset_estimate,
                            /* forced_bfc_block_offset */ std::nullopt);

      if ((bfc_offset_already_resolved || has_adjoining_floats) &&
          old_offset != *container_builder_.BfcBlockOffset()) {
        // The first BFC block offset resolution turned out to be wrong, and we
        // positioned preceding adjacent floats based on that. Now we have to
        // roll back and position them at the correct offset. The only expected
        // incorrect estimate is with the child's margin adjoining. Any other
        // incorrect estimate will result in failed layout.
        DCHECK_EQ(old_offset, adjoining_bfc_offset_estimate);
        return LayoutResult::kBfcBlockOffsetResolved;
      }
    }

    child_bfc_offset_estimate = non_adjoining_bfc_offset_estimate;
    child_margin_got_separated = true;

    // We can re-layout the child right away. This re-layout *must* produce a
    // fragment which fits within the exclusion space.
    layout_result = LayoutNewFormattingContext(
        child, child_break_token, child_data,
        {child_origin_line_offset, child_bfc_offset_estimate},
        /* abort_if_cleared */ false, &child_bfc_offset, &resolved_margins);
  }

  if (constraint_space.HasBlockFragmentation()) {
    bool has_container_separation =
        has_break_opportunity_before_next_child_ ||
        child_bfc_offset.block_offset > child_bfc_offset_estimate ||
        layout_result->IsPushedByFloats();
    BreakStatus break_status = BreakBeforeChildIfNeeded(
        child, *layout_result, previous_inflow_position,
        child_bfc_offset.block_offset, has_container_separation);
    if (break_status == BreakStatus::kBrokeBefore) {
      return LayoutResult::kSuccess;
    }
    if (break_status == BreakStatus::kNeedsEarlierBreak) {
      return LayoutResult::kNeedsEarlierBreak;
    }

    // If the child aborted layout, we cannot continue.
    DCHECK_EQ(layout_result->Status(), LayoutResult::kSuccess);
  }

  const auto& physical_fragment = layout_result->GetPhysicalFragment();
  LogicalFragment fragment(constraint_space.GetWritingDirection(),
                           physical_fragment);

  LogicalOffset logical_offset = LogicalFromBfcOffsets(
      child_bfc_offset, ContainerBfcOffset(), fragment.InlineSize(),
      container_builder_.InlineSize(), constraint_space.Direction());

  if (!PositionOrPropagateListMarker(*layout_result, &logical_offset,
                                     previous_inflow_position))
    return LayoutResult::kBfcBlockOffsetResolved;

  PropagateBaselineFromBlockChild(physical_fragment, resolved_margins,
                                  logical_offset.block_offset);

  container_builder_.AddResult(*layout_result, logical_offset,
                               resolved_margins);

  if (!child_break_token || !child_break_token->IsInParallelFlow()) {
    *previous_inflow_position = ComputeInflowPosition(
        *previous_inflow_position, child, child_data,
        child_bfc_offset.block_offset, logical_offset, *layout_result, fragment,
        /* self_collapsing_child_had_clearance */ false);
  }

  // Update line-clamp data, and abort if needed
  if (!line_clamp_data_.UpdateAfterLayout(layout_result, Node().GetDocument(),
                                          *container_builder_.BfcBlockOffset(),
                                          *previous_inflow_position,
                                          Padding().block_end)) {
    container_builder_.SetLinesUntilClamp(
        line_clamp_data_.LinesUntilClamp(/*show_measured_lines*/ true));
    container_builder_.SetLineClampAfterLayoutObject(
        line_clamp_data_.last_layout_object);
    return LayoutResult::kNeedsLineClampRelayout;
  }

  if (container_builder_.ShouldTextBoxTrim()) {
    UpdateTextBoxTrim(child, child_break_token,
                      /*outgoing_inline_break_token=*/nullptr, layout_result,
                      previous_inflow_position);
  }

  if (constraint_space.HasBlockFragmentation() &&
      !has_break_opportunity_before_next_child_) {
    has_break_opportunity_before_next_child_ =
        HasBreakOpportunityBeforeNextChild(physical_fragment,
                                           child_break_token);
  }

  return LayoutResult::kSuccess;
}

const LayoutResult* BlockLayoutAlgorithm::LayoutNewFormattingContext(
    LayoutInputNode child,
    const BlockBreakToken* child_break_token,
    const InflowChildData& child_data,
    BfcOffset origin_offset,
    bool abort_if_cleared,
    BfcOffset* out_child_bfc_offset,
    BoxStrut* out_resolved_margins) {
  const auto& style = Style();
  const auto& child_style = child.Style();
  const TextDirection direction = GetConstraintSpace().Direction();
  const auto writing_direction = GetConstraintSpace().GetWritingDirection();

  if (!IsBreakInside(child_break_token)) {
    // The origin offset is where we should start looking for layout
    // opportunities. It needs to be adjusted by the child's clearance.
    AdjustToClearance(GetExclusionSpace().ClearanceOffsetIncludingInitialLetter(
                          child_style.Clear(style)),
                      &origin_offset);
  }
  DCHECK(container_builder_.BfcBlockOffset());

  LayoutOpportunityVector opportunities =
      GetExclusionSpace().AllLayoutOpportunities(
          origin_offset, ChildAvailableSize().inline_size);
  ClearCollectionScope scope(&opportunities);

  // We should always have at least one opportunity.
  DCHECK_GT(opportunities.size(), 0u);

  // Now we lay out. This will give us a child fragment and thus its size, which
  // means that we can find out if it's actually going to fit. If it doesn't
  // fit where it was laid out, and is pushed downwards, we'll lay out over
  // again, since a new BFC block offset could result in a new fragment size,
  // e.g. when inline size is auto, or if we're block-fragmented.
  for (const auto& opportunity : opportunities) {
    if (abort_if_cleared &&
        origin_offset.block_offset < opportunity.rect.BlockStartOffset()) {
      // Abort if we got pushed downwards. We need to adjust
      // origin_offset.block_offset, reposition any floats affected by that, and
      // try again.
      return nullptr;
    }

    // Determine which sides of the opportunity have floats we should avoid.
    // We can detect this when the opportunity-rect sides match the
    // available-rect sides.
    bool has_floats_on_line_left =
        opportunity.rect.LineStartOffset() != origin_offset.line_offset;
    bool has_floats_on_line_right =
        opportunity.rect.LineEndOffset() !=
        (origin_offset.line_offset + ChildAvailableSize().inline_size);
    bool can_expand_outside_opportunity =
        !has_floats_on_line_left && !has_floats_on_line_right;

    const LayoutUnit line_left_margin = child_data.margins.LineLeft(direction);
    const LayoutUnit line_right_margin =
        child_data.margins.LineRight(direction);

    // Find the available inline-size which should be given to the child.
    LayoutUnit line_left_offset = opportunity.rect.LineStartOffset();
    LayoutUnit line_right_offset = opportunity.rect.LineEndOffset();

    if (can_expand_outside_opportunity) {
      // No floats have affected the available inline-size, adjust the
      // available inline-size by the margins.
      DCHECK_EQ(line_left_offset, origin_offset.line_offset);
      DCHECK_EQ(line_right_offset,
                origin_offset.line_offset + ChildAvailableSize().inline_size);
      line_left_offset += line_left_margin;
      line_right_offset -= line_right_margin;
    } else {
      // Margins are applied from the content-box, not the layout opportunity
      // area. Instead of adjusting by the size of the margins, we "shrink" the
      // available inline-size if required.
      line_left_offset = std::max(
          line_left_offset,
          origin_offset.line_offset + line_left_margin.ClampNegativeToZero());
      line_right_offset = std::min(line_right_offset,
                                   origin_offset.line_offset +
                                       ChildAvailableSize().inline_size -
                                       line_right_margin.ClampNegativeToZero());
    }
    LayoutUnit opportunity_size =
        (line_right_offset - line_left_offset).ClampNegativeToZero();

    // The available inline size in the child constraint space needs to include
    // inline margins, since layout algorithms (both legacy and NG) will resolve
    // auto inline size by subtracting the inline margins from available inline
    // size. We have calculated a layout opportunity without margins in mind,
    // since they overlap with adjacent floats. Now we need to add them.
    LayoutUnit child_available_inline_size =
        (opportunity_size + child_data.margins.InlineSum())
            .ClampNegativeToZero();

    ConstraintSpace child_space = CreateConstraintSpaceForChild(
        child, child_break_token, child_data,
        {child_available_inline_size, ChildAvailableSize().block_size},
        /* is_new_fc */ true, opportunity.rect.start_offset.block_offset);

    // All formatting context roots (like this child) should start with an empty
    // exclusion space.
    DCHECK(child_space.GetExclusionSpace().IsEmpty());

    const LayoutResult* layout_result = LayoutBlockChild(
        child_space, child_break_token, early_break_,
        /* column_spanner_path */ nullptr, &To<BlockNode>(child));

    // Since this child establishes a new formatting context, no exclusion space
    // should be returned.
    DCHECK(layout_result->GetExclusionSpace().IsEmpty());

    DCHECK_EQ(layout_result->Status(), LayoutResult::kSuccess);

    // Check if we can fit in the opportunity block direction.
    LogicalFragment fragment(writing_direction,
                             layout_result->GetPhysicalFragment());
    if (fragment.BlockSize() > opportunity.rect.BlockSize())
      continue;

    // Now find the fragment's (final) position calculating the auto margins.
    BoxStrut auto_margins = child_data.margins;
    LayoutUnit text_align_offset;
    bool has_auto_margins = false;
    if (child.IsListMarker()) {
      // Deal with marker's margin. It happens only when marker needs to occupy
      // the whole line.
      DCHECK(child.ListMarkerOccupiesWholeLine());
      // Because the marker is laid out as a normal block child, its inline
      // size is extended to fill up the space. Compute the regular marker size
      // from the first child.
      const auto& marker_fragment =
          To<PhysicalBoxFragment>(layout_result->GetPhysicalFragment());
      LayoutUnit marker_inline_size;
      if (!marker_fragment.Children().empty()) {
        marker_inline_size =
            LogicalFragment(writing_direction,
                            *marker_fragment.Children().front())
                .InlineSize();
      }
      auto_margins.inline_start = UnpositionedListMarker(To<BlockNode>(child))
                                      .InlineOffset(marker_inline_size);
      auto_margins.inline_end = opportunity.rect.InlineSize() -
                                fragment.InlineSize() -
                                auto_margins.inline_start;
    } else {
      if (child_style.MarginInlineStartUsing(style).IsAuto() ||
          child_style.MarginInlineEndUsing(style).IsAuto()) {
        has_auto_margins = true;
        ResolveInlineAutoMargins(child_style, style,
                                 child_available_inline_size,
                                 fragment.InlineSize(), &auto_margins);
      } else {
        // Handle -webkit- values for text-align.
        text_align_offset = WebkitTextAlignAndJustifySelfOffset(
            child_style, style, opportunity.rect.InlineSize(),
            child_data.margins, [&]() { return fragment.InlineSize(); });
      }
    }

    // Determine our final BFC offset.
    //
    // NOTE: |auto_margins| are initialized as a copy of the child's initial
    // margins. To determine the effect of the auto-margins we apply only the
    // difference.
    BfcOffset child_bfc_offset = {LayoutUnit(),
                                  opportunity.rect.BlockStartOffset()};
    if (direction == TextDirection::kLtr) {
      LayoutUnit auto_margin_line_left =
          auto_margins.LineLeft(direction) - line_left_margin;
      child_bfc_offset.line_offset =
          line_left_offset + auto_margin_line_left + text_align_offset;
    } else {
      LayoutUnit auto_margin_line_right =
          auto_margins.LineRight(direction) - line_right_margin;
      child_bfc_offset.line_offset = line_right_offset - text_align_offset -
                                     auto_margin_line_right -
                                     fragment.InlineSize();
    }

    // Check if we'll intersect any floats on our line-left/line-right.
    if (has_floats_on_line_left &&
        child_bfc_offset.line_offset < opportunity.rect.LineStartOffset())
      continue;
    if (has_floats_on_line_right &&
        child_bfc_offset.line_offset + fragment.InlineSize() >
            opportunity.rect.LineEndOffset())
      continue;

    // If we can't expand outside our opportunity, check if we fit in the
    // inline direction.
    if (!can_expand_outside_opportunity &&
        fragment.InlineSize() > opportunity.rect.InlineSize())
      continue;

    // auto-margins are "fun". To ensure round tripping from getComputedStyle
    // the used values are relative to the content-box edge, rather than the
    // opportunity edge.
    BoxStrut resolved_margins = child_data.margins;
    if (has_auto_margins) {
      LayoutUnit inline_offset =
          LogicalFromBfcLineOffset(child_bfc_offset.line_offset,
                                   container_builder_.BfcLineOffset(),
                                   fragment.InlineSize(),
                                   container_builder_.InlineSize(), direction) -
          BorderScrollbarPadding().inline_start;
      if (child_style.MarginInlineStartUsing(style).IsAuto()) {
        resolved_margins.inline_start = inline_offset;
      }
      if (child_style.MarginInlineEndUsing(style).IsAuto()) {
        resolved_margins.inline_end = ChildAvailableSize().inline_size -
                                      inline_offset - fragment.InlineSize();
      }
    }

    *out_child_bfc_offset = child_bfc_offset;
    *out_resolved_margins = resolved_margins;
    return layout_result;
  }

  NOTREACHED();
}

LayoutResult::EStatus BlockLayoutAlgorithm::HandleInflow(
    LayoutInputNode child,
    const BreakToken* child_break_token,
    PreviousInflowPosition* previous_inflow_position,
    InlineChildLayoutContext* inline_child_layout_context,
    const InlineBreakToken** previous_inline_break_token) {
  IpidDepthLog ipid_depth_log("BlockLayoutAlgorithm::HandleInflow");

  ipid_depth_log.FPrint(
      "开始处理流入布局的子元素 {}。\n"
      "子元素类型：{}\n"
      "子元素断开令牌：{}\n"
      "之前的流入位置：logical_block_offset={}，margin_strut={}，block_end_"
      "annotation_space={}\n"
      "之前的内联断开令牌：{}",
      ipid::GetNodeStr(child), child.IsBlock() ? "block" : "inline",
      child_break_token ? "存在" : "不存在",
      previous_inflow_position->logical_block_offset,
      ipid::GetMarginStrutString(previous_inflow_position->margin_strut),
      previous_inflow_position->block_end_annotation_space,
      *previous_inline_break_token ? "存在" : "不存在");

  DCHECK(child);
  DCHECK(!child.IsFloating());
  DCHECK(!child.IsOutOfFlowPositioned());
  DCHECK(!child.CreatesNewFormattingContext());

  auto* child_inline_node = DynamicTo<InlineNode>(child);
  if (child_inline_node) {
    ipid_depth_log.FPrint("检测到子元素 {} 是内联节点，尝试重用缓存中的行框。",
                          ipid::GetNodeStr(child));
    // Add reusable line boxes from |previous_result_| if any.
    if (!abort_when_bfc_block_offset_updated_ && !child_break_token &&
        previous_result_) {
      ipid_depth_log.FPrint(
          "条件满足（未中止BFC块偏移更新、无子元素断开令牌、有前一结果），"
          "尝试从缓存重用片段。");
      DCHECK(!*previous_inline_break_token);
      if (TryReuseFragmentsFromCache(*child_inline_node,
                                     previous_inflow_position,
                                     previous_inline_break_token)) {
        ipid_depth_log.FPrint(
            "成功从缓存重用片段，返回 LayoutResult::kSuccess。");
        return LayoutResult::kSuccess;
      } else {
        ipid_depth_log.FPrint("未能从缓存重用片段，继续正常处理。");
      }
    } else {
      ipid_depth_log.FPrint("条件不满足，无法重用缓存片段，继续正常处理。");
    }
  } else {
    ipid_depth_log.FPrint("子元素 {} 是块节点，直接进入正常处理流程。",
                          ipid::GetNodeStr(child));
  }

  bool has_clearance_past_adjoining_floats =
      !container_builder_.BfcBlockOffset() && child.IsBlock() &&
      HasClearancePastAdjoiningFloats(
          container_builder_.GetAdjoiningObjectTypes(), child.Style(), Style());

  ipid_depth_log.FPrint(
      "检查子元素 {} 是否需要清除相邻浮动。\n"
      "条件检查：BFC 块偏移为：{}，子元素为：{}"
      "，HasClearancePastAdjoiningFloats={"
      "}\n"
      "最终结果：{}",
      ipid::GetNodeStr(child),
      container_builder_.BfcBlockOffset().value_or(LayoutUnit(-114514)),
      child.IsBlock() ? "块级元素（BlockNode）" : "行内元素（InlineNode）",
      HasClearancePastAdjoiningFloats(
          container_builder_.GetAdjoiningObjectTypes(), child.Style(), Style())
          ? "true"
          : "false",
      has_clearance_past_adjoining_floats ? "需要清除" : "不需要清除");

  std::optional<LayoutUnit> forced_bfc_block_offset;
  bool is_pushed_by_floats = false;

  // If we can separate the previous margin strut from what is to follow, do
  // that. Then we're able to resolve *our* BFC block offset and position any
  // pending floats. There are two situations where this is necessary:
  //  1. If the child is to be cleared by adjoining floats.
  //  2. If the child is a non-empty inline.
  //
  // Note this logic is copied to TryReuseFragmentsFromCache(), they need to
  // keep in sync.
  if (has_clearance_past_adjoining_floats) {
    ipid_depth_log.FPrint("子元素 {} 需要清除相邻浮动，开始解析 BFC 块偏移。",
                          ipid::GetNodeStr(child));
    if (!ResolveBfcBlockOffset(previous_inflow_position)) {
      ipid_depth_log.FPrint(
          "BFC 块偏移解析失败，返回 LayoutResult::kBfcBlockOffsetResolved。");
      return LayoutResult::kBfcBlockOffsetResolved;
    }
    ipid_depth_log.FPrint("BFC 块偏移解析成功。");

    // If we had clearance past any adjoining floats, we already know where the
    // child is going to be (the child's margins won't have any effect).
    //
    // Set the forced BFC block-offset to the appropriate clearance offset to
    // force this placement of this child.
    forced_bfc_block_offset =
        GetExclusionSpace().ClearanceOffset(child.Style().Clear(Style()));
    is_pushed_by_floats = true;

    ipid_depth_log.FPrint("设置强制 BFC 块偏移为 {}px，标记子元素被浮动推移。",
                          forced_bfc_block_offset.value());
  } else {
    ipid_depth_log.FPrint(
        "子元素 {} 不需要清除相邻浮动，跳过BFC块偏移解析步骤。",
        ipid::GetNodeStr(child));
  }

  // Perform layout on the child.
  ipid_depth_log.FPrint("开始计算子元素 {} 的数据和约束空间。",
                        ipid::GetNodeStr(child));
  InflowChildData child_data =
      ComputeChildData(*previous_inflow_position, child, child_break_token,
                       /* is_new_fc */ false);
  child_data.is_pushed_by_floats = is_pushed_by_floats;

  ipid_depth_log.FPrint("子元素数据计算完成，is_pushed_by_floats = {}。",
                        child_data.is_pushed_by_floats);

  ConstraintSpace child_space = CreateConstraintSpaceForChild(
      child, child_break_token, child_data, ChildAvailableSize(),
      /* is_new_fc */ false, forced_bfc_block_offset,
      has_clearance_past_adjoining_floats,
      previous_inflow_position->block_end_annotation_space);

  ipid_depth_log.FPrint("为子元素 {} 创建的约束空间：{}。",
                        ipid::GetNodeStr(child),
                        ipid::GetConstraintSpaceString(child_space));

  const LayoutResult* layout_result =
      LayoutInflow(child_space, child_break_token, early_break_,
                   column_spanner_path_, &child, inline_child_layout_context);

  ipid_depth_log.FPrint(
      "子元素 {} 布局完成，布局结果状态：{}。", ipid::GetNodeStr(child),
      ipid::GetLayoutResultStatusString(layout_result->Status()));

  // To save space of the stack when we recurse into |BlockNode::Layout|
  // above, the rest of this function is continued within |FinishInflow|.
  // However it should be read as one function.
  ipid_depth_log.FPrint(
      "HandleInflow 处理完成，调用 FinishInflow 继续后续逻辑。\n"
      "传递给 FinishInflow 的参数：\n"
      "  子元素：{}\n"
      "  子元素断开令牌：{}\n"
      "  约束空间：{}\n"
      "  是否清除相邻浮动：{}\n"
      "  布局结果状态：{}\n"
      "  子元素数据：is_pushed_by_floats={}\n"
      "  之前的流入位置：logical_block_offset={}\n"
      "  之前的内联断开令牌：{}",
      ipid::GetNodeStr(child), child_break_token ? "存在" : "不存在",
      ipid::GetConstraintSpaceString(child_space),
      has_clearance_past_adjoining_floats ? "是" : "否",
      ipid::GetLayoutResultStatusString(layout_result->Status()),
      child_data.is_pushed_by_floats,
      previous_inflow_position->logical_block_offset,
      *previous_inline_break_token ? "存在" : "不存在");

  return FinishInflow(child, child_break_token, child_space,
                      has_clearance_past_adjoining_floats,
                      std::move(layout_result), &child_data,
                      previous_inflow_position, inline_child_layout_context,
                      previous_inline_break_token);
}

LayoutResult::EStatus BlockLayoutAlgorithm::FinishInflow(
    LayoutInputNode child,
    const BreakToken* child_break_token,
    const ConstraintSpace& child_space,
    bool has_clearance_past_adjoining_floats,
    const LayoutResult* layout_result,
    InflowChildData* child_data,
    PreviousInflowPosition* previous_inflow_position,
    InlineChildLayoutContext* inline_child_layout_context,
    const InlineBreakToken** previous_inline_break_token) {
  IpidDepthLog ipid_depth_log("BlockLayoutAlgorithm::FinishInflow");

  ipid_depth_log.FPrint(
      "开始完成子元素 {} 的内联布局。\n"
      "子元素布局结果状态：{}\n"
      "子元素是否为自折叠：{}\n"
      "开辟的空间：{}",
      ipid::GetNodeStr(child),
      ipid::GetLayoutResultStatusString(layout_result->Status()),
      ipid::btos(layout_result->IsSelfCollapsing()),
      ipid::GetConstraintSpaceString(child_space));

  // If a kNeedsLineClampRelayout layout result was not handled in
  // HandleNonSuccessfulLayoutResult, it needs to be propagated upwards until
  // the BFC root.
  if (layout_result->Status() == LayoutResult::kNeedsLineClampRelayout) {
    ipid_depth_log.FPrint(
        "子元素布局结果状态为 kNeedsLineClampRelayout（需要行夹重新布局），"
        "这表示行夹逻辑需要重新布局，必须向上传播到 BFC 根元素。"
        "设置 LinesUntilClamp 为 {}，传播后的布局对象为 {}。",
        layout_result->LinesUntilClamp(),
        ipid::GetNodeStr(
            line_clamp_data_.PropagateClampAfterLayoutObject(layout_result)));
    DCHECK(line_clamp_data_.data.IsMeasureUntilBfcOffset());
    container_builder_.SetLinesUntilClamp(layout_result->LinesUntilClamp());
    container_builder_.SetLineClampAfterLayoutObject(
        line_clamp_data_.PropagateClampAfterLayoutObject(layout_result));
    ipid_depth_log.FPrint("向上传播 kNeedsLineClampRelayout 状态。");
    return LayoutResult::kNeedsLineClampRelayout;
  }

  std::optional<LayoutUnit> child_bfc_block_offset =
      layout_result->BfcBlockOffset();
  ipid_depth_log.FPrint("子元素的 BFC 块偏移为：{}",
                        child_bfc_block_offset
                            ? std::to_string(child_bfc_block_offset->ToInt())
                            : "nullopt");

  bool is_self_collapsing = layout_result->IsSelfCollapsing();
  ipid_depth_log.FPrint("子元素是否为自折叠：{}",
                        ipid::btos(is_self_collapsing));

  // "Normal child" here means non-self-collapsing. Even self-collapsing
  // children may be cleared by floats, if they have a forced BFC block-offset.
  bool normal_child_had_clearance =
      layout_result->IsPushedByFloats() && !is_self_collapsing;
  ipid_depth_log.FPrint(
      "普通子元素是否被浮动推下（有清除）：{}，"
      "layout_result->IsPushedByFloats() 为 {}，"
      "!is_self_collapsing 为 {}。",
      ipid::btos(normal_child_had_clearance),
      ipid::btos(layout_result->IsPushedByFloats()),
      ipid::btos(!is_self_collapsing));

  // A child may have aborted its layout if it resolved its BFC block-offset.
  // If we don't have a BFC block-offset yet, we need to propagate the abort
  // signal up to our parent.
  if (layout_result->Status() == LayoutResult::kBfcBlockOffsetResolved &&
      !container_builder_.BfcBlockOffset()) {
    ipid_depth_log.FPrint(
        "子元素布局状态为 kBfcBlockOffsetResolved（已解析 BFC 块偏移），"
        "且当前容器尚未解析 BFC 块偏移，需要向上传播终止信号。");
    // There's no need to do anything apart from resolving the BFC block-offset
    // here, so make sure that it aborts before trying to position floats or
    // anything like that, which would just be waste of time.
    //
    // This is simply propagating an abort up to a node which is able to
    // restart the layout (a node that has resolved its BFC block-offset).
    DCHECK(child_bfc_block_offset);
    abort_when_bfc_block_offset_updated_ = true;

    LayoutUnit bfc_block_offset = *child_bfc_block_offset;
    bool old_abort_flag = abort_when_bfc_block_offset_updated_;
    abort_when_bfc_block_offset_updated_ = true;
    ipid_depth_log.FPrint(
        "设置终止标志为 true（之前为 {}），并使用子元素的 BFC 块偏移：{}",
        ipid::btos(old_abort_flag), bfc_block_offset);

    if (normal_child_had_clearance) {
      // If the child has the same clearance-offset as ourselves it means that
      // we should *also* resolve ourselves at that offset, (and we also have
      // been pushed by floats).
      if (GetConstraintSpace().ClearanceOffset() ==
          child_space.ClearanceOffset()) {
        ipid_depth_log.FPrint(
            "普通子元素有清除，且清除偏移与当前容器相同，"
            "因此将当前容器也标记为被浮动推下。");
        container_builder_.SetIsPushedByFloats();
      } else {
        LayoutUnit next_border_edge = NextBorderEdge(*previous_inflow_position);
        ipid_depth_log.FPrint(
            "普通子元素有清除但偏移不同，将 BFC 块偏移调整为下一个边框边缘：{}",
            next_border_edge);
        bfc_block_offset = next_border_edge;
      }
    }

    // A new formatting-context may have previously tried to resolve the BFC
    // block-offset. In this case we'll have a "forced" BFC block-offset
    // present, but we shouldn't apply it (instead preferring the child's new
    // BFC block-offset).
    ipid_depth_log.FPrint(
        "检查祖先是否有清除过去的相邻浮动：{}",
        ipid::btos(
            GetConstraintSpace().AncestorHasClearancePastAdjoiningFloats()));
    DCHECK(!GetConstraintSpace().AncestorHasClearancePastAdjoiningFloats());

    ipid_depth_log.FPrint("尝试解析当前容器的 BFC 块偏移为：{}",
                          bfc_block_offset);
    if (!ResolveBfcBlockOffset(previous_inflow_position, bfc_block_offset,
                               /* forced_bfc_block_offset */ std::nullopt)) {
      ipid_depth_log.FPrint(
          "解析 BFC 块偏移失败，返回 kBfcBlockOffsetResolved。");
      return LayoutResult::kBfcBlockOffsetResolved;
    }
    ipid_depth_log.FPrint(
        "成功解析 BFC 块偏移为：{}。",
        container_builder_.BfcBlockOffset()
            ? std::to_string(container_builder_.BfcBlockOffset()->ToInt())
            : "nullopt");
  }

  // We have special behavior for a self-collapsing child which gets pushed
  // down due to clearance, see comment inside |ComputeInflowPosition|.
  bool self_collapsing_child_had_clearance =
      is_self_collapsing && has_clearance_past_adjoining_floats;
  ipid_depth_log.FPrint("自折叠子元素是否因清除而被推下：{}",
                        ipid::btos(self_collapsing_child_had_clearance));

  // We try and position the child within the block formatting-context. This
  // may cause our BFC block-offset to be resolved, in which case we should
  // abort our layout if needed.
  if (!child_bfc_block_offset) {
    DCHECK(is_self_collapsing);
    ipid_depth_log.FPrint(
        "子元素是自折叠的且尚未确定 BFC 块偏移，检查清除逻辑。");
    if (child_space.HasClearanceOffset() && child.Style().HasClear()) {
      // This is a self-collapsing child that we collapsed through, so we have
      // to detect clearance manually. See if the child's hypothetical border
      // edge is past the relevant floats. If it's not, we need to apply
      // clearance before it.
      LayoutUnit child_block_offset_estimate =
          BfcBlockOffset() + layout_result->EndMarginStrut().Sum();
      ipid_depth_log.FPrint(
          "计算子元素的假设块偏移：当前 BFC 块偏移 {} + 结束边距 strut 总和 {} "
          "= {}",
          BfcBlockOffset(), layout_result->EndMarginStrut().Sum(),
          child_block_offset_estimate);
      if (child_block_offset_estimate < child_space.ClearanceOffset()) {
        ipid_depth_log.FPrint(
            "假设块偏移 {} 小于清除偏移 {}，因此自折叠子元素有清除。",
            child_block_offset_estimate, child_space.ClearanceOffset());
        self_collapsing_child_had_clearance = true;
      }
    }
  }

  bool child_had_clearance =
      self_collapsing_child_had_clearance || normal_child_had_clearance;
  ipid_depth_log.FPrint("子元素最终是否有清除：{}",
                        ipid::btos(child_had_clearance));
  if (child_had_clearance) {
    // The child has clearance. Clearance inhibits margin collapsing and acts as
    // spacing before the block-start margin of the child. Our BFC block offset
    // is therefore resolvable, and if it hasn't already been resolved, we'll
    // do it now to separate the child's collapsed margin from this container.
    ipid_depth_log.FPrint(
        "子元素有清除，清除会阻止边距折叠并作为子元素块起始边距之前的间距，"
        "因此需要解析当前容器的 BFC 块偏移。");
    if (!ResolveBfcBlockOffset(previous_inflow_position)) {
      ipid_depth_log.FPrint(
          "解析 BFC 块偏移失败，返回 kBfcBlockOffsetResolved。");
      return LayoutResult::kBfcBlockOffsetResolved;
    }
    ipid_depth_log.FPrint(
        "成功解析 BFC 块偏移为：{}。",
        container_builder_.BfcBlockOffset()
            ? std::to_string(container_builder_.BfcBlockOffset()->ToInt())
            : "nullopt");
  } else if (layout_result->SubtreeModifiedMarginStrut()) {
    // The child doesn't have clearance, and modified its incoming
    // margin-strut. Propagate this information up to our parent if needed.
    ipid_depth_log.FPrint(
        "子元素没有清除，但修改了其传入的边距 "
        "strut（{}），需要向上传播此信息。",
        ipid::btos(layout_result->SubtreeModifiedMarginStrut()));
    SetSubtreeModifiedMarginStrutIfNeeded();
  }

  bool self_collapsing_child_needs_relayout = false;
  if (!child_bfc_block_offset) {
    // Layout wasn't able to determine the BFC block-offset of the child. This
    // has to mean that the child is self-collapsing.
    DCHECK(is_self_collapsing);
    ipid_depth_log.FPrint("子元素是自折叠的且 BFC 块偏移未知。");

    if (container_builder_.BfcBlockOffset() &&
        layout_result->Status() == LayoutResult::kSuccess) {
      // Since we know our own BFC block-offset, though, we can calculate that
      // of the child as well.
      ipid_depth_log.FPrint(
          "容器已有 BFC 块偏移且布局成功，计算自折叠子元素的 BFC 块偏移。");
      child_bfc_block_offset = PositionSelfCollapsingChildWithParentBfc(
          child, child_space, *child_data, *layout_result);
      ipid_depth_log.FPrint(
          "计算得到子元素的 BFC 块偏移：{}",
          child_bfc_block_offset
              ? std::to_string(child_bfc_block_offset->ToInt())
              : "nullopt");

      // We may need to relayout this child if it had any (adjoining) objects
      // which were positioned in the incorrect place.
      if (layout_result->GetPhysicalFragment()
              .HasAdjoiningObjectDescendants() &&
          *child_bfc_block_offset != child_space.ExpectedBfcBlockOffset()) {
        ipid_depth_log.FPrint(
            "子元素有相邻对象后代，且计算的 BFC 块偏移 {} 与期望的 {} 不匹配，"
            "需要重新布局自折叠子元素。",
            *child_bfc_block_offset, child_space.ExpectedBfcBlockOffset());
        self_collapsing_child_needs_relayout = true;
      }
    }
  } else if (!child_had_clearance && !is_self_collapsing) {
    // Only non self-collapsing children are allowed resolve their parent's BFC
    // block-offset. We check the BFC block-offset at the end of layout
    // determine if this fragment is self-collapsing.
    //
    // The child's BFC block-offset is known, and since there's no clearance,
    // this container will get the same offset, unless it has already been
    // resolved.
    ipid_depth_log.FPrint(
        "子元素是非自折叠的且没有清除，其 BFC 块偏移已知 {}，"
        "尝试使用此偏移解析当前容器的 BFC 块偏移。",
        *child_bfc_block_offset);
    if (!ResolveBfcBlockOffset(previous_inflow_position,
                               *child_bfc_block_offset)) {
      ipid_depth_log.FPrint(
          "解析 BFC 块偏移失败，返回 kBfcBlockOffsetResolved。");
      return LayoutResult::kBfcBlockOffsetResolved;
    }
    ipid_depth_log.FPrint(
        "成功解析 BFC 块偏移为：{}。",
        container_builder_.BfcBlockOffset()
            ? std::to_string(container_builder_.BfcBlockOffset()->ToInt())
            : "nullopt");
  }

  // We need to re-layout a self-collapsing child if it was affected by
  // clearance in order to produce a new margin strut. For example:
  // <div style="margin-bottom: 50px;"></div>
  // <div id="float" style="height: 50px;"></div>
  // <div id="zero" style="clear: left; margin-top: -20px;">
  //   <div id="zero-inner" style="margin-top: 40px; margin-bottom: -30px;">
  // </div>
  //
  // The end margin strut for #zero will be {50, -30}. #zero will be affected
  // by clearance (as 50 > {50, -30}).
  //
  // As #zero doesn't touch the incoming margin strut now we need to perform a
  // relayout with an empty incoming margin strut.
  //
  // The resulting margin strut in the above example will be {40, -30}. See
  // |ComputeInflowPosition| for how this end margin strut is used.
  if (self_collapsing_child_had_clearance) {
    ipid_depth_log.FPrint(
        "自折叠子元素因清除而被推下，需要重新布局以产生新的边距 strut。");
    MarginStrut margin_strut;
    margin_strut.Append(child_data->margins.block_start,
                        child.Style().HasMarginBlockStartQuirk());
    ipid_depth_log.FPrint(
        "创建新的边距 strut，追加块起始边距 {}（是否为 quirk 模式：{}）。",
        child_data->margins.block_start,
        ipid::btos(child.Style().HasMarginBlockStartQuirk()));

    // We only need to relayout if the new margin strut is different to the
    // previous one.
    if (child_data->margin_strut != margin_strut) {
      ipid_depth_log.FPrint(
          "新的边距 strut {} 与之前的 {} 不同，需要重新布局。",
          ipid::GetMarginStrutString(margin_strut),
          ipid::GetMarginStrutString(child_data->margin_strut));
      child_data->margin_strut = margin_strut;
      self_collapsing_child_needs_relayout = true;
      ipid_depth_log.FPrint(
          "设置 self_collapsing_child_needs_relayout 为 true。");
    } else {
      ipid_depth_log.FPrint("边距 strut 没有变化，无需重新布局。");
    }
  }

  // We need to layout a child if we know its BFC block offset and:
  //  - It aborted its layout as it resolved its BFC block offset.
  //  - It has some unpositioned floats.
  //  - It was affected by clearance.
  if ((layout_result->Status() == LayoutResult::kBfcBlockOffsetResolved ||
       self_collapsing_child_needs_relayout) &&
      child_bfc_block_offset) {
    ipid_depth_log.FPrint(
        "子元素需要重新布局：状态为 kBfcBlockOffsetResolved "
        "或需要重新布局自折叠子元素（{}），"
        "且已知 BFC 块偏移 {}。",
        ipid::btos(self_collapsing_child_needs_relayout),
        *child_bfc_block_offset);
    // Assert that any clearance previously detected isn't lost.
    DCHECK(!child_data->is_pushed_by_floats ||
           layout_result->IsPushedByFloats());
    // If the child got pushed down by floats (normally because of clearance),
    // we need to carry over this state to the next layout pass, as clearance
    // won't automatically be detected then, since the BFC block-offset will
    // already be past the relevant floats.
    ipid_depth_log.FPrint("将子元素被浮动推下的状态 {} 传递到下一次布局。",
                          ipid::btos(layout_result->IsPushedByFloats()));
    child_data->is_pushed_by_floats = layout_result->IsPushedByFloats();

    ipid_depth_log.FPrint("为子元素创建新的约束空间，使用 BFC 块偏移 {}。",
                          *child_bfc_block_offset);
    ConstraintSpace new_child_space = CreateConstraintSpaceForChild(
        child, child_break_token, *child_data, ChildAvailableSize(),
        /* is_new_fc */ false, child_bfc_block_offset);
    ipid_depth_log.FPrint("使用新的约束空间 {} 重新布局子元素。",
                          ipid::GetConstraintSpaceString(new_child_space));
    layout_result =
        LayoutInflow(new_child_space, child_break_token, early_break_,
                     column_spanner_path_, &child, inline_child_layout_context);
    ipid_depth_log.FPrint(
        "重新布局完成，新的布局结果状态：{}",
        ipid::GetLayoutResultStatusString(layout_result->Status()));

    if (layout_result->Status() == LayoutResult::kBfcBlockOffsetResolved) {
      ipid_depth_log.FPrint(
          "第二次布局仍返回 kBfcBlockOffsetResolved，允许第三次布局。");
      // Even a second layout pass may abort, if the BFC block offset initially
      // calculated turned out to be wrong. This happens when we discover that
      // an in-flow block-level descendant that establishes a new formatting
      // context doesn't fit beside the floats at its initial position. Allow
      // one more pass.
      child_bfc_block_offset = layout_result->BfcBlockOffset();
      DCHECK(child_bfc_block_offset);
      ipid_depth_log.FPrint("更新子元素 BFC 块偏移为 {}。",
                            *child_bfc_block_offset);

      // We don't expect clearance to be detected at this point. Any clearance
      // should already have been detected above.
      DCHECK(child_data->is_pushed_by_floats ||
             !layout_result->IsPushedByFloats());

      ipid_depth_log.FPrint("为第三次布局创建新的约束空间。");
      new_child_space = CreateConstraintSpaceForChild(
          child, child_break_token, *child_data, ChildAvailableSize(),
          /* is_new_fc */ false, child_bfc_block_offset);
      ipid_depth_log.FPrint("执行第三次布局。");
      layout_result = LayoutInflow(new_child_space, child_break_token,
                                   early_break_, column_spanner_path_, &child,
                                   inline_child_layout_context);
      ipid_depth_log.FPrint(
          "第三次布局完成，状态：{}",
          ipid::GetLayoutResultStatusString(layout_result->Status()));
    }

    // If a kNeedsLineClampRelayout layout result was not handled in
    // HandleNonSuccessfulLayoutResult, it needs to be propagated upwards until
    // the BFC root.
    if (layout_result->Status() == LayoutResult::kNeedsLineClampRelayout) {
      ipid_depth_log.FPrint("重新布局后仍需要行夹重新布局，向上传播状态。");
      DCHECK(line_clamp_data_.data.IsMeasureUntilBfcOffset());
      container_builder_.SetLinesUntilClamp(layout_result->LinesUntilClamp());
      container_builder_.SetLineClampAfterLayoutObject(
          line_clamp_data_.PropagateClampAfterLayoutObject(layout_result));
      return LayoutResult::kNeedsLineClampRelayout;
    }

    ipid_depth_log.FPrint("重新布局成功完成。");
    DCHECK_EQ(layout_result->Status(), LayoutResult::kSuccess);

    // We stored this in a local variable, so it better not have changed.
    DCHECK_EQ(layout_result->IsSelfCollapsing(), is_self_collapsing);
  }

  const std::optional<LayoutUnit> line_box_bfc_block_offset =
      layout_result->LineBoxBfcBlockOffset();
  ipid_depth_log.FPrint("行框 BFC 块偏移：{}",
                        line_box_bfc_block_offset
                            ? std::to_string(line_box_bfc_block_offset->ToInt())
                            : "nullopt");

  if (GetConstraintSpace().HasBlockFragmentation()) {
    ipid_depth_log.FPrint("当前空间有块分片，检查是否需要在子元素前断开。");
    // If the BFC block-offset is known both for this container and for the
    // child, breaking before may be possible, unless this is a resumed inline
    // formatting context in a parallel block flow. There are situations where
    // such parallel flows cannot be resumed, due to a float (that got pushed
    // from a previous fragmentainer) taking up all the available space in the
    // current fragmentainer, for instance. In such cases we'll just repropagate
    // the break tokens, by obtaining them from inline_child_layout_context
    // below.
    bool consider_breaking_before =
        container_builder_.BfcBlockOffset() && child_bfc_block_offset &&
        (!child.IsInline() || !child_break_token ||
         !To<InlineBreakToken>(child_break_token)->IsInParallelBlockFlow());
    ipid_depth_log.FPrint("是否考虑在子元素前断开：{}",
                          ipid::btos(consider_breaking_before));

    if (consider_breaking_before) {
      bool is_line_box_pushed_by_floats =
          line_box_bfc_block_offset &&
          *line_box_bfc_block_offset > *child_bfc_block_offset;
      ipid_depth_log.FPrint("行框是否被浮动推下：{}",
                            ipid::btos(is_line_box_pushed_by_floats));

      // Floats only cause container separation for the outermost block child
      // that gets pushed down (the container and the child may have adjoining
      // block-start margins).
      bool has_container_separation =
          has_break_opportunity_before_next_child_ ||
          (!container_builder_.IsPushedByFloats() &&
           (layout_result->IsPushedByFloats() || is_line_box_pushed_by_floats));
      ipid_depth_log.FPrint("是否有容器分离：{}",
                            ipid::btos(has_container_separation));

      // If this is a line with a block-in-inline, use the result for the
      // block-in-inline instead of that for the line. That's where we find the
      // relevant info for block fragmentation considerations, including the
      // block break token, if any.
      const LayoutResult& layout_result_to_use =
          container_builder_.LayoutResultForPropagation(*layout_result);
      ipid_depth_log.FPrint("使用传播布局结果进行断开检查。");

      LayoutUnit break_offset =
          line_box_bfc_block_offset.value_or(*child_bfc_block_offset);
      ipid_depth_log.FPrint("断开偏移：{}", break_offset);
      BreakStatus break_status = BreakBeforeChildIfNeeded(
          child, layout_result_to_use, previous_inflow_position,
          line_box_bfc_block_offset.value_or(*child_bfc_block_offset),
          has_container_separation);

      if (child_space.ShouldForceTextBoxTrimEnd()) {
        ipid_depth_log.FPrint("强制文本框修剪结束，清除修剪标志。");
        // This is the line that we decided to come back to and trim, as an
        // attempt to fit it in the fragmentainer. This may or may not have
        // succeeded, but in any case, we can stop looking for a place to trim.
        container_builder_.ClearShouldTextBoxTrimEnd();
      }

      if (break_status == BreakStatus::kBrokeBefore) {
        ipid_depth_log.FPrint("已在子元素前断开。");
        // The line didn't fit, but if trimming is enabled, try again by
        // trimming the block-end side of the line box. It might fit
        // then. Otherwise we'll get here again and break before it.
        if (container_builder_.ShouldTextBoxTrimFragmentainerEnd() &&
            child.IsInline() && !child_space.ShouldForceTextBoxTrimEnd()) {
          ipid_depth_log.FPrint("启用文本框修剪，设置最后非空内联子元素。");
          last_non_empty_inflow_child_ = To<InlineNode>(child);
          last_non_empty_break_token_ = child_break_token;
          return LayoutResult::kTextBoxTrimEndDidNotApply;
        } else {
          ipid_depth_log.FPrint("清除文本框修剪标志。");
          container_builder_.ClearShouldTextBoxTrimEnd();
        }
        return LayoutResult::kSuccess;
      }

      if (break_status == BreakStatus::kNeedsEarlierBreak) {
        ipid_depth_log.FPrint("需要更早的断开。");
        return LayoutResult::kNeedsEarlierBreak;
      }
    }

    if (inline_child_layout_context) {
      for (const auto& token :
           inline_child_layout_context->ParallelFlowBreakTokens()) {
        container_builder_.AddBreakToken(token.Get(),
                                         /* is_in_parallel_flow */ true);
      }
    }
  }

  // It is now safe to update our version of the exclusion space, and any
  // propagated adjoining floats.
  ipid_depth_log.FPrint("更新排除空间和相邻对象类型。");
  container_builder_.SetExclusionSpace(layout_result->GetExclusionSpace());

  // Only self-collapsing children should have adjoining objects.
  DCHECK(!layout_result->GetAdjoiningObjectTypes() || is_self_collapsing);
  container_builder_.SetAdjoiningObjectTypes(
      layout_result->GetAdjoiningObjectTypes());

  // If we don't know our BFC block-offset yet, and the child stumbled into
  // something that needs it (unable to position floats yet), we need abort
  // layout, and trigger a re-layout once we manage to resolve it.
  //
  // NOTE: This check is performed after the optional second layout pass above,
  // since we may have been able to resolve our BFC block-offset (e.g. due to
  // clearance) and position any descendant floats in the second pass.
  // In particular, when it comes to clearance of self-collapsing children, if
  // we just applied it and resolved the BFC block-offset to separate the
  // margins before and after clearance, we cannot abort and re-layout this
  // child, or clearance would be lost.
  //
  // If we are a new formatting context, the child will get re-laid out once it
  // has been positioned.
  if (!container_builder_.BfcBlockOffset()) {
    ipid_depth_log.FPrint(
        "当前容器 BFC "
        "块偏移未知，如果子元素有相邻对象类型，需要设置终止标志。");
    abort_when_bfc_block_offset_updated_ |=
        layout_result->GetAdjoiningObjectTypes();
    // If our BFC block offset is unknown, and the child got pushed down by
    // floats, so will we.
    if (layout_result->IsPushedByFloats()) {
      ipid_depth_log.FPrint("子元素被浮动推下，设置当前容器也为被浮动推下。");
      container_builder_.SetIsPushedByFloats();
    }
  }

  const auto& physical_fragment = layout_result->GetPhysicalFragment();
  LogicalFragment fragment(GetConstraintSpace().GetWritingDirection(),
                           physical_fragment);
  ipid_depth_log.FPrint("创建逻辑片段，类型：{}",
                        physical_fragment.IsLineBox() ? "行框" : "块");

  if (line_box_bfc_block_offset) {
    ipid_depth_log.FPrint("使用行框 BFC 块偏移 {} 覆盖子元素 BFC 块偏移。",
                          *line_box_bfc_block_offset);
    child_bfc_block_offset = line_box_bfc_block_offset;
  }

  LogicalOffset logical_offset = CalculateLogicalOffset(
      fragment, layout_result->BfcLineOffset(), child_bfc_block_offset);
  ipid_depth_log.FPrint("计算逻辑偏移：内联 {}，块 {}",
                        logical_offset.inline_offset,
                        logical_offset.block_offset);
  if (child.IsSliderThumb()) [[unlikely]] {
    ipid_depth_log.FPrint("调整滑块拇指的内联偏移。");
    logical_offset = AdjustSliderThumbInlineOffset(fragment, logical_offset);
  }

  if (!PositionOrPropagateListMarker(*layout_result, &logical_offset,
                                     previous_inflow_position)) {
    ipid_depth_log.FPrint("列表标记定位失败，返回 kBfcBlockOffsetResolved。");
    return LayoutResult::kBfcBlockOffsetResolved;
  }

  if (physical_fragment.IsLineBox()) {
    ipid_depth_log.FPrint("从行框传播基线。");
    PropagateBaselineFromLineBox(physical_fragment,
                                 logical_offset.block_offset);
  } else {
    ipid_depth_log.FPrint("从块子元素传播基线。");
    PropagateBaselineFromBlockChild(physical_fragment, child_data->margins,
                                    logical_offset.block_offset);
  }

  if (IsA<BlockNode>(child)) {
    ipid_depth_log.FPrint("将块子元素结果添加到容器构建器。");
    container_builder_.AddResult(*layout_result, logical_offset,
                                 child_data->margins);
  } else {
    ipid_depth_log.FPrint("将内联子元素结果添加到容器构建器。");
    container_builder_.AddResult(*layout_result, logical_offset);
  }

  if (!child_break_token || !child_break_token->IsInParallelFlow()) {
    ipid_depth_log.FPrint("计算下一个内联位置。");
    *previous_inflow_position = ComputeInflowPosition(
        *previous_inflow_position, child, *child_data, child_bfc_block_offset,
        logical_offset, *layout_result, fragment,
        self_collapsing_child_had_clearance);
  }

  const InlineBreakToken* outgoing_inline_break_token = nullptr;
  if (child.IsInline()) {
    outgoing_inline_break_token =
        To<InlineBreakToken>(physical_fragment.GetBreakToken());
    ipid_depth_log.FPrint("获取内联断开标记。");
  }
  *previous_inline_break_token = outgoing_inline_break_token;

  // Update |line_clamp_data_| from the LayoutResult, and abort if needed.
  // If the BFC block offset hasn't been resolved, the child we just laid out
  // must be empty (no lines and zero block size), so we can skip the update.
  if (auto bfc_block_offset = container_builder_.BfcBlockOffset()) {
    if (!line_clamp_data_.UpdateAfterLayout(
            layout_result, Node().GetDocument(), *bfc_block_offset,
            *previous_inflow_position, Padding().block_end)) {
      container_builder_.SetLinesUntilClamp(
          line_clamp_data_.LinesUntilClamp(/*show_measured_lines*/ true));
      container_builder_.SetLineClampAfterLayoutObject(
          line_clamp_data_.last_layout_object);
      return LayoutResult::kNeedsLineClampRelayout;
    }
  }

  if (container_builder_.ShouldTextBoxTrim()) [[unlikely]] {
    ipid_depth_log.FPrint("更新文本框修剪。");
    UpdateTextBoxTrim(child, child_break_token, outgoing_inline_break_token,
                      layout_result, previous_inflow_position);
  }

  if (GetConstraintSpace().HasBlockFragmentation() &&
      !has_break_opportunity_before_next_child_) {
    ipid_depth_log.FPrint("检查下一个子元素前的断开机会。");
    has_break_opportunity_before_next_child_ =
        HasBreakOpportunityBeforeNextChild(physical_fragment,
                                           child_break_token);
  }

  ipid_depth_log.FPrint("FinishInflow 完成，返回 kSuccess。");
  return LayoutResult::kSuccess;
}

void BlockLayoutAlgorithm::UpdateTextBoxTrim(
    LayoutInputNode child,
    const BreakToken* incoming_child_break_token,
    const InlineBreakToken* outgoing_inline_break_token,
    const LayoutResult* layout_result,
    PreviousInflowPosition* previous_inflow_position) {
  container_builder_.ClearShouldTextBoxTrimFragmentainerStart();
  // Update text box trimming flags if the child `layout_result` has applied
  // `text-box-trim`, or was meant to apply it.
  if (container_builder_.ShouldTextBoxTrimNodeStart()) {
    if (!child.IsInline() || !outgoing_inline_break_token ||
        outgoing_inline_break_token->IsPastFirstFormattedLine()) {
      container_builder_.ClearShouldTextBoxTrimNodeStart();
    }
  }
  if (container_builder_.ShouldTextBoxTrimNodeEnd()) {
    if (line_clamp_data_.data.IsMeasureUntilBfcOffset() &&
        layout_result->TrimBlockEndBy() &&
        layout_result->GetPhysicalFragment().GetBreakToken()) {
      // If we trimmed the end only because we're in the first layout of a
      // line-clamp: auto context, and we might not trim in the relayout, then
      // we don't clear ShouldTextBoxTrimNodeEnd, and we add the trim length to
      // the logical block offset so next lines are set in the right position.
      previous_inflow_position->logical_block_offset +=
          *layout_result->TrimBlockEndBy();
    } else if (layout_result->IsBlockEndTrimmableLine() ||
               (child.IsBlock() && IsLastInflowChild(*child.GetLayoutBox()))) {
      container_builder_.ClearShouldTextBoxTrimEnd();
    } else if (!layout_result->IsSelfCollapsing() && child.IsInline() &&
               !override_text_box_trim_end_child_) {
      // Keep the last non-empty child for `RelayoutForTextBoxTrimEnd`.
      last_non_empty_inflow_child_ = To<InlineNode>(child);
      last_non_empty_break_token_ = incoming_child_break_token;
    }
  }
}

InflowChildData BlockLayoutAlgorithm::ComputeChildData(
    const PreviousInflowPosition& previous_inflow_position,
    LayoutInputNode child,
    const BreakToken* child_break_token,
    bool is_new_fc) {
  DCHECK(child);
  DCHECK(!child.IsFloating());
  DCHECK_EQ(is_new_fc, child.CreatesNewFormattingContext());

  // Calculate margins in parent's writing mode.
  LayoutUnit additional_line_offset;
  BoxStrut margins =
      CalculateMargins(child, is_new_fc, &additional_line_offset);

  // Append the current margin strut with child's block start margin.
  // Non empty border/padding, and new formatting-context use cases are handled
  // inside of the child's layout
  MarginStrut margin_strut = previous_inflow_position.margin_strut;

  LayoutUnit logical_block_offset =
      previous_inflow_position.logical_block_offset;

  const auto* child_block_break_token =
      DynamicTo<BlockBreakToken>(child_break_token);
  if (child_block_break_token) [[unlikely]] {
    AdjustMarginsForFragmentation(child_block_break_token, &margins);
    if (child_block_break_token->IsForcedBreak()) {
      // After a forced fragmentainer break we need to reset the margin strut,
      // in case it was set to discard all margins (which is the default at
      // breaks). Margins after a forced break should be retained.
      margin_strut = MarginStrut();
    }

    if (child_block_break_token->MonolithicOverflow() &&
        (Node().IsPaginatedRoot() || !GetBreakToken()->MonolithicOverflow())) {
      // Every container that needs to be pushed to steer clear of monolithic
      // overflow on a previous page will have this stored in its break token.
      // So we'll only add the additional offset here if the child is the
      // outermost container with monolithic overflow recorded.
      logical_block_offset += child_block_break_token->MonolithicOverflow();
    }
  }

  margin_strut.Append(margins.block_start,
                      child.Style().HasMarginBlockStartQuirk());
  if (child.IsBlock())
    SetSubtreeModifiedMarginStrutIfNeeded(&child.Style().MarginBlockStart());

  TextDirection direction = GetConstraintSpace().Direction();
  BfcOffset child_bfc_offset = {
      GetConstraintSpace().GetBfcOffset().line_offset +
          BorderScrollbarPadding().LineLeft(direction) +
          additional_line_offset + margins.LineLeft(direction),
      BfcBlockOffset() + logical_block_offset};

  return InflowChildData(child_bfc_offset, margin_strut, margins);
}

PreviousInflowPosition BlockLayoutAlgorithm::ComputeInflowPosition(
    const PreviousInflowPosition& previous_inflow_position,
    const LayoutInputNode child,
    const InflowChildData& child_data,
    const std::optional<LayoutUnit>& child_bfc_block_offset,
    const LogicalOffset& logical_offset,
    const LayoutResult& layout_result,
    const LogicalFragment& fragment,
    bool self_collapsing_child_had_clearance) {
  // Determine the child's end logical offset, for the next child to use.
  LayoutUnit logical_block_offset;
  std::optional<LayoutUnit> clearance_after_line;
  std::optional<LayoutUnit> trim_block_end_by;

  const bool is_self_collapsing = layout_result.IsSelfCollapsing();
  if (is_self_collapsing) {
    // The default behavior for self-collapsing children is they just pass
    // through the previous inflow position.
    logical_block_offset = previous_inflow_position.logical_block_offset;

    if (self_collapsing_child_had_clearance) {
      // If there's clearance, we must have applied that by now and thus
      // resolved our BFC block-offset.
      DCHECK(container_builder_.BfcBlockOffset());
      DCHECK(child_bfc_block_offset.has_value());

      // If a self-collapsing child was affected by clearance (that is it got
      // pushed down past a float), we need to do something slightly bizarre.
      //
      // Instead of just passing through the previous inflow position, we make
      // the inflow position our new position (which was affected by the
      // float), minus what the margin strut which the self-collapsing child
      // produced.
      //
      // Another way of thinking about this is that when you *add* back the
      // margin strut, you end up with the same position as you started with.
      //
      // This is essentially what the spec refers to as clearance [1], and,
      // while we normally don't have to calculate it directly, in the case of
      // a self-collapsing cleared child like here, we actually have to.
      //
      // We have to calculate clearance for self-collapsing cleared children,
      // because we need the margin that's between the clearance and this block
      // to collapse correctly with subsequent content. This is something that
      // needs to take place after the margin strut preceding and following the
      // clearance have been separated. Clearance may be positive, negative or
      // zero, depending on what it takes to (hypothetically) place this child
      // just below the last relevant float. Since the margins before and after
      // the clearance have been separated, we may have to pull the child back,
      // and that's an example of negative clearance.
      //
      // (In the other case, when a cleared child is non self-collapsing (i.e.
      // when we don't end up here), we don't need to explicitly calculate
      // clearance, because then we just place its border edge where it should
      // be and we're done with it.)
      //
      // [1] https://www.w3.org/TR/CSS22/visuren.html#flow-control

      // First move past the margin that is to precede the clearance. It will
      // not participate in any subsequent margin collapsing.
      LayoutUnit margin_before_clearance =
          previous_inflow_position.margin_strut.Sum();
      logical_block_offset += margin_before_clearance;

      // Calculate and apply actual clearance.
      LayoutUnit clearance = *child_bfc_block_offset -
                             layout_result.EndMarginStrut().Sum() -
                             NextBorderEdge(previous_inflow_position);
      logical_block_offset += clearance;
    }
    if (!container_builder_.BfcBlockOffset())
      DCHECK_EQ(logical_block_offset, LayoutUnit());
  } else {
    logical_block_offset = logical_offset.block_offset + fragment.BlockSize();

    clearance_after_line = layout_result.ClearanceAfterLine();
    trim_block_end_by = layout_result.TrimBlockEndBy();
    if (trim_block_end_by) {
      // Trim the space to respect the `text-box-trim` property here. Objects
      // that pushes following boxes down (e.g., Ruby annotations) are also
      // trimmed.
      logical_block_offset -= *trim_block_end_by;

      if (clearance_after_line) {
        // `<br>` with clearance is an exception. It still pushes down, after
        // all other objects are trimmed. See `AddAnyClearanceAfterLine()`.
        logical_block_offset += *clearance_after_line;
      }
    } else {
      // We add the greater of AnnotationOverflow and ClearanceAfterLine here.
      // Then, we cancel the AnnotationOverflow part if
      //  - The next line box has block-start annotation space, or
      //  - There are no following child boxes and this container has block-end
      //    padding.
      //
      // See InlineLayoutAlgorithm::CreateLine() and
      // BlockLayoutAlgorithm::Layout().
      logical_block_offset +=
          std::max(layout_result.AnnotationOverflow(),
                   clearance_after_line.value_or(LayoutUnit()));
    }
  }

  MarginStrut margin_strut = layout_result.EndMarginStrut();

  // Self collapsing child's end margin can "inherit" quirkiness from its start
  // margin. E.g.
  // <ol style="margin-bottom: 20px"></ol>
  bool is_quirky =
      (is_self_collapsing && child.Style().HasMarginBlockStartQuirk()) ||
      child.Style().HasMarginBlockEndQuirk();
  margin_strut.Append(child_data.margins.block_end, is_quirky);
  if (child.IsBlock())
    SetSubtreeModifiedMarginStrutIfNeeded(&child.Style().MarginBlockEnd());

  if (GetConstraintSpace().HasBlockFragmentation()) [[unlikely]] {
    // If the child broke inside, don't apply any trailing margin, since it's
    // only to be applied to the last fragment that's not in a parallel flow
    // (due to overflow). While trailing margins are normally truncated at
    // fragmentainer boundaries, so that whether or not we add such margins
    // doesn't really make much of a difference, this isn't the case in the
    // initial column balancing pass.
    if (const auto* physical_fragment = DynamicTo<PhysicalBoxFragment>(
            &layout_result.GetPhysicalFragment())) {
      if (const BlockBreakToken* token = physical_fragment->GetBreakToken()) {
        // TODO(mstensho): Don't apply the margin to all overflowing fragments
        // (if any). It should only be applied after the fragment where we
        // reached the block-end of the node.
        if (!token->IsAtBlockEnd())
          margin_strut = MarginStrut();
      }
    }
  }

  // This flag is subtle, but in order to determine our size correctly we need
  // to check if our last child is self-collapsing, and it was affected by
  // clearance *or* an adjoining self-collapsing sibling was affected by
  // clearance. E.g.
  // <div id="container">
  //   <div id="float"></div>
  //   <div id="zero-with-clearance"></div>
  //   <div id="another-zero"></div>
  // </div>
  // In the above case #container's size will depend on the end margin strut of
  // #another-zero, even though usually it wouldn't.
  bool self_or_sibling_self_collapsing_child_had_clearance =
      self_collapsing_child_had_clearance ||
      (previous_inflow_position.self_collapsing_child_had_clearance &&
       is_self_collapsing);

  LayoutUnit annotation_space;
  if (!is_self_collapsing && !trim_block_end_by) {
    annotation_space = layout_result.BlockEndAnnotationSpace();
    if (layout_result.AnnotationOverflow() > LayoutUnit()) {
      DCHECK(!annotation_space);
      // Allow the portion of the annotation overflow that isn't also part of
      // clearance to overlap with certain types of subsequent content.
      annotation_space = -std::max(
          LayoutUnit(), layout_result.AnnotationOverflow() -
                            clearance_after_line.value_or(LayoutUnit()));
    }
  }

  return {logical_block_offset, margin_strut, annotation_space,
          self_or_sibling_self_collapsing_child_had_clearance};
}

LayoutUnit BlockLayoutAlgorithm::PositionSelfCollapsingChildWithParentBfc(
    const LayoutInputNode& child,
    const ConstraintSpace& child_space,
    const InflowChildData& child_data,
    const LayoutResult& layout_result) const {
  DCHECK(layout_result.IsSelfCollapsing());

  // The child must be an in-flow zero-block-size fragment, use its end margin
  // strut for positioning.
  LayoutUnit child_bfc_block_offset =
      child_data.bfc_offset_estimate.block_offset +
      layout_result.EndMarginStrut().Sum();

  ApplyClearance(child_space, &child_bfc_block_offset);

  return child_bfc_block_offset;
}

void BlockLayoutAlgorithm::ConsumeRemainingFragmentainerSpace(
    PreviousInflowPosition* previous_inflow_position) {
  if (GetConstraintSpace().HasKnownFragmentainerBlockSize()) {
    // The remaining part of the fragmentainer (the unusable space for child
    // content, due to the break) should still be occupied by this
    // container. Also encompass fragmentainer overflow (may be caused by
    // monolithic content).
    previous_inflow_position->logical_block_offset =
        std::max(previous_inflow_position->logical_block_offset,
                 FragmentainerSpaceLeftForChildren());
  }
}

BreakStatus BlockLayoutAlgorithm::FinalizeForFragmentation() {
  if (Node().IsTableCell()) {
    // For table cells, prevent breaking before trailing box decorations, as
    // that might disturb the row stretching machinery, causing an infinite
    // loop. We'd add the stretch amount to the block-size to the content box of
    // the table cell, even though we're past it.
    container_builder_.SetShouldPreventBreakBeforeBlockEndDecorations(true);
  }

  if (Node().IsInlineFormattingContextRoot() && !early_break_ &&
      GetConstraintSpace().HasBlockFragmentation()) {
    if (container_builder_.HasInflowChildBreakInside() ||
        first_overflowing_line_) {
      if (first_overflowing_line_ &&
          first_overflowing_line_ < container_builder_.LineCount()) {
        int line_number;
        if (fit_all_lines_) {
          line_number = first_overflowing_line_;
        } else {
          // We managed to finish layout of all the lines for the node, which
          // means that we won't have enough widows, unless we break earlier
          // than where we overflowed.
          int line_count = container_builder_.LineCount();
          line_number = std::max(line_count - Style().Widows(),
                                 std::min(line_count, int(Style().Orphans())));
        }
        // We need to layout again, and stop at the right line number.
        const auto* breakpoint =
            MakeGarbageCollected<EarlyBreak>(line_number, kBreakAppealPerfect);
        container_builder_.SetEarlyBreak(breakpoint);
        return BreakStatus::kNeedsEarlierBreak;
      }
    } else {
      // Everything could fit in the current fragmentainer, but, depending on
      // what comes after, the best location to break at may be between two of
      // our lines.
      UpdateEarlyBreakBetweenLines();
    }
  }

  if (container_builder_.IsFragmentainerBoxType()) {
    return FinishFragmentationForFragmentainer(&container_builder_);
  }

  return FinishFragmentation(&container_builder_);
}

BreakStatus BlockLayoutAlgorithm::BreakBeforeChildIfNeeded(
    LayoutInputNode child,
    const LayoutResult& layout_result,
    PreviousInflowPosition* previous_inflow_position,
    LayoutUnit bfc_block_offset,
    bool has_container_separation) {
  DCHECK(GetConstraintSpace().HasBlockFragmentation());

  // If the BFC offset is unknown, there's nowhere to break, since there's no
  // non-empty child content yet (as that would have resolved the BFC offset).
  DCHECK(container_builder_.BfcBlockOffset());

  LayoutUnit fragmentainer_block_offset =
      FragmentainerOffsetAtBfc(container_builder_) + bfc_block_offset -
      layout_result.AnnotationBlockOffsetAdjustment();

  if (has_container_separation) {
    EBreakBetween break_between =
        CalculateBreakBetweenValue(child, layout_result, container_builder_);
    if (IsForcedBreakValue(GetConstraintSpace(), break_between)) {
      BreakBeforeChild(GetConstraintSpace(), child, &layout_result,
                       fragmentainer_block_offset,
                       FragmentainerCapacityForChildren(), kBreakAppealPerfect,
                       /* is_forced_break */ true, &container_builder_);
      ConsumeRemainingFragmentainerSpace(previous_inflow_position);
      return BreakStatus::kBrokeBefore;
    }
  }

  BreakAppeal appeal_before =
      CalculateBreakAppealBefore(GetConstraintSpace(), child, layout_result,
                                 container_builder_, has_container_separation);

  // Attempt to move past the break point, and if we can do that, also assess
  // the appeal of breaking there, even if we didn't.
  if (MovePastBreakpoint(child, layout_result, fragmentainer_block_offset,
                         appeal_before)) {
    return BreakStatus::kContinue;
  }

  // Figure out where to insert a soft break. It will either be before this
  // child, or before an earlier sibling, if there's a more appealing breakpoint
  // there.

  // Handle line boxes - propagate space shortage and attempt to honor orphans
  // and widows (or detect violations). Skip this part if we didn't produce a
  // fragment (status != kSuccess). The latter happens with BR clear=all if we
  // need to push it to a later fragmentainer to get past floats. BR clear="all"
  // adds clearance *after* the contents (the line), unlike regular CSS
  // clearance, which adds clearance *before* the contents). To handle this
  // corner-case as simply as possible, we'll break (line-wise AND block-wise)
  // before a BR clear=all element, and add it in the fragmentainer where the
  // relevant floats end. This means that we might get an additional line box
  // (to simply hold the BR clear=all), that should be ignored as far as orphans
  // and widows are concerned. Just give up instead, and break before it.
  //
  // Orphans and widows affect column balancing, and if we get imperfect breaks
  // (such as widows / orphans violations), we'll attempt to stretch the
  // columns, and without this exception for BR clear=all, we'd end up
  // stretching to fit the entire float(s) (that could otherwise be broken
  // nicely into fragments) in a single column.
  if (child.IsInline() && layout_result.Status() == LayoutResult::kSuccess) {
    if (!first_overflowing_line_) {
      // We're at the first overflowing line. This is the space shortage that
      // we are going to report. We do this in spite of not yet knowing
      // whether breaking here would violate orphans and widows requests. This
      // approach may result in a lower space shortage than what's actually
      // true, which leads to more layout passes than we'd otherwise
      // need. However, getting this optimal for orphans and widows would
      // require an additional piece of machinery. This case should be rare
      // enough (to worry about performance), so let's focus on code
      // simplicity instead.
      PropagateSpaceShortage(
          GetConstraintSpace(), &layout_result, fragmentainer_block_offset,
          FragmentainerCapacityForChildren(), &container_builder_);
    }
    // Attempt to honor orphans and widows requests.
    if (int line_count = container_builder_.LineCount()) {
      if (!first_overflowing_line_)
        first_overflowing_line_ = line_count;
      bool is_first_fragment = !GetBreakToken();
      // Figure out how many lines we need before the break. That entails to
      // attempt to honor the orphans request.
      int minimum_line_count = Style().Orphans();
      if (!is_first_fragment) {
        // If this isn't the first fragment, it means that there's a break both
        // before and after this fragment. So what was seen as trailing widows
        // in the previous fragment is essentially orphans for us now.
        minimum_line_count =
            std::max(minimum_line_count, static_cast<int>(Style().Widows()));
      }
      if (line_count < minimum_line_count) {
        // Not enough orphans. Our only hope is if we can break before the start
        // of this block to improve on the situation. That's not something we
        // can determine at this point though. Permit the break, but mark it as
        // undesirable.
        if (appeal_before > kBreakAppealViolatingOrphansAndWidows)
          appeal_before = kBreakAppealViolatingOrphansAndWidows;
      } else {
        // There are enough lines before the break. Try to make sure that
        // there'll be enough lines after the break as well. Attempt to honor
        // the widows request.
        DCHECK_GE(line_count, first_overflowing_line_);
        int widows_found = line_count - first_overflowing_line_ + 1;
        if (widows_found < Style().Widows()) {
          // Although we're out of space, we have to continue layout to figure
          // out exactly where to break in order to honor the widows
          // request. We'll make sure that we're going to leave at least as many
          // lines as specified by the 'widows' property for the next fragment
          // (if at all possible), which means that lines that could fit in the
          // current fragment (that we have already laid out) may have to be
          // saved for the next fragment.
          //
          // However, any text box block-end trimming must take place before
          // calculating widows, since we might fit an additional line by
          // trimming.
          if (!container_builder_.ShouldTextBoxTrimFragmentainerEnd() ||
              override_text_box_trim_end_child_) {
            return BreakStatus::kContinue;
          }
        }

        // We have determined that there are plenty of lines for the next
        // fragment, so we can just break exactly where we ran out of space,
        // rather than pushing some of the line boxes over to the next fragment.
      }
      fit_all_lines_ = true;
    }
  }

  if (!AttemptSoftBreak(GetConstraintSpace(), child, &layout_result,
                        fragmentainer_block_offset,
                        FragmentainerCapacityForChildren(), appeal_before,
                        &container_builder_)) {
    return BreakStatus::kNeedsEarlierBreak;
  }

  ConsumeRemainingFragmentainerSpace(previous_inflow_position);
  return BreakStatus::kBrokeBefore;
}

void BlockLayoutAlgorithm::UpdateEarlyBreakBetweenLines() {
  // We shouldn't be here if we already know where to break.
  DCHECK(!early_break_);

  // If something in this flow already broke, it's a little too late to look for
  // breakpoints.
  DCHECK(!container_builder_.HasInflowChildBreakInside());

  int line_count = container_builder_.LineCount();
  if (line_count < 2)
    return;
  // We can break between two of the lines if we have to. Calculate the best
  // line number to break before, and the appeal of such a breakpoint.
  int line_number =
      std::max(line_count - Style().Widows(),
               std::min(line_count - 1, static_cast<int>(Style().Orphans())));
  BreakAppeal appeal = kBreakAppealPerfect;
  if (line_number < Style().Orphans() ||
      line_count - line_number < Style().Widows()) {
    // Not enough lines in this container to satisfy the orphans and/or widows
    // requirement. If we break before the last line (i.e. the last possible
    // class B breakpoint), we'll fit as much as possible, and that's the best
    // we can do.
    line_number = line_count - 1;
    appeal = kBreakAppealViolatingOrphansAndWidows;
  }
  if (container_builder_.HasEarlyBreak() &&
      container_builder_.GetEarlyBreak().GetBreakAppeal() > appeal) {
    return;
  }
  const auto* breakpoint =
      MakeGarbageCollected<EarlyBreak>(line_number, appeal);
  container_builder_.SetEarlyBreak(breakpoint);
}

BoxStrut BlockLayoutAlgorithm::CalculateMargins(
    LayoutInputNode child,
    bool is_new_fc,
    LayoutUnit* additional_line_offset) {
  DCHECK(child);
  if (child.IsInline())
    return {};

  const ComputedStyle& child_style = child.Style();
  BoxStrut margins =
      ComputeMarginsFor(child_style, child_percentage_size_.inline_size,
                        GetConstraintSpace().GetWritingDirection());
  if (is_new_fc) {
    return margins;
  }

  std::optional<LayoutUnit> child_inline_size;
  auto ChildInlineSize = [&]() -> LayoutUnit {
    if (!child_inline_size) {
      ConstraintSpaceBuilder builder(GetConstraintSpace(),
                                     child_style.GetWritingDirection(),
                                     /* is_new_fc */ false);
      builder.SetAvailableSize(ChildAvailableSize());
      builder.SetPercentageResolutionSize(child_percentage_size_);

      const ItemPosition justify_self =
          child_style
              .ResolvedJustifySelf(
                  {ItemPosition::kNormal, OverflowAlignment::kDefault},
                  &Style())
              .GetPosition();

      if (child.IsAnonymousBlockFlow()) {
        builder.SetInlineAutoBehavior(AutoSizeBehavior::kStretchImplicit);
      } else if (justify_self == ItemPosition::kStretch) {
        builder.SetInlineAutoBehavior(AutoSizeBehavior::kStretchExplicit);
      } else if (justify_self != ItemPosition::kNormal) {
        builder.SetInlineAutoBehavior(AutoSizeBehavior::kFitContent);
      } else {
        builder.SetInlineAutoBehavior(AutoSizeBehavior::kStretchImplicit);
      }
      ConstraintSpace space = builder.ToConstraintSpace();

      const auto block_child = To<BlockNode>(child);
      BoxStrut child_border_padding = ComputeBorders(space, block_child) +
                                      ComputePadding(space, child_style);
      child_inline_size = ComputeInlineSizeForFragment(space, block_child,
                                                       child_border_padding);
    }
    return *child_inline_size;
  };

  const auto& style = Style();
  const bool is_rtl = IsRtl(style.Direction());
  const LayoutUnit available_space = ChildAvailableSize().inline_size;

  LayoutUnit text_align_offset;
  if (child_style.MarginInlineStartUsing(style).IsAuto() ||
      child_style.MarginInlineEndUsing(style).IsAuto()) {
    // Resolve auto-margins.
    ResolveInlineAutoMargins(child_style, style, available_space,
                             ChildInlineSize(), &margins);
  } else {
    // Handle -webkit- values for text-align.
    text_align_offset = WebkitTextAlignAndJustifySelfOffset(
        child_style, style, available_space, margins, ChildInlineSize);
  }

  if (is_rtl) {
    *additional_line_offset = ChildAvailableSize().inline_size -
                              text_align_offset - ChildInlineSize() -
                              margins.InlineSum();
  } else {
    *additional_line_offset = text_align_offset;
  }

  return margins;
}

ConstraintSpace BlockLayoutAlgorithm::CreateConstraintSpaceForChild(
    const LayoutInputNode child,
    const BreakToken* child_break_token,
    const InflowChildData& child_data,
    const LogicalSize child_available_size,
    bool is_new_fc,
    const std::optional<LayoutUnit> child_bfc_block_offset,
    bool has_clearance_past_adjoining_floats,
    LayoutUnit block_start_annotation_space) {
  IpidDepthLog ipid_depth_log("BlockLayoutAlgorithm::CreateConstraintSpaceForChild");

  const ComputedStyle& child_style = child.Style();
  const auto child_writing_direction = child_style.GetWritingDirection();
  const auto& constraint_space = GetConstraintSpace();

  ipid_depth_log.FPrint(
      "为子元素 {} 创建约束空间。\n"
      "子元素可用大小：{}（来自调用方传入）\n"
      "是否为新的格式化上下文：{}（来自调用方传入）\n"
      "父约束空间：{}\n"
      "子元素数据：BFC偏移预估 {}，margin strut {}，margins {}（来自 ComputeChildData）",
      ipid::GetNodeStr(child), ipid::GetLogicalSizeString(child_available_size),
      ipid::btos(is_new_fc), ipid::GetConstraintSpaceString(constraint_space),
      child_data.bfc_offset_estimate, ipid::GetMarginStrutString(child_data.margin_strut),
      ipid::GetBoxStrutString(child_data.margins));

  ConstraintSpaceBuilder builder(constraint_space, child_writing_direction,
                                 is_new_fc);
  ipid_depth_log.FPrint("创建了 ConstraintSpaceBuilder，准备配置子元素的约束空间参数。");

  const bool is_in_parallel_flow =
      IsParallelWritingMode(constraint_space.GetWritingMode(),
                            child_writing_direction.GetWritingMode());
  ipid_depth_log.FPrint(
      "检查子元素的 writing-mode 是否与父元素平行：{}\n"
      "父元素 writing-mode：{}，子元素 writing-mode：{}",
      ipid::btos(is_in_parallel_flow),
      static_cast<int>(constraint_space.GetWritingMode()),
      static_cast<int>(child_writing_direction.GetWritingMode()));
  
  if (!is_in_parallel_flow) [[unlikely]] {
    ipid_depth_log.FPrint(
        "子元素的 writing-mode 与父元素正交，设置正交回退宽度。");
    SetOrthogonalFallbackInlineSize(Style(), child, &builder);
  }

  if (child.IsInline()) {
    ipid_depth_log.FPrint("子元素是内联元素。");
    if (is_in_parallel_flow) {
      ipid_depth_log.FPrint(
          "内联元素且 writing-mode 平行，设置 auto 宽度行为为 kStretchImplicit（隐式拉伸）。");
      builder.SetInlineAutoBehavior(AutoSizeBehavior::kStretchImplicit);
    }
  } else {
    ipid_depth_log.FPrint("子元素是块级元素，分析其 justify-self 属性来决定 auto 宽度行为。");
    
    const ItemPosition justify_self =
        child_style
            .ResolvedJustifySelf(
                {ItemPosition::kNormal, OverflowAlignment::kDefault}, &Style())
            .GetPosition();
    ipid_depth_log.FPrint(
        "子元素的 justify-self 解析结果：{}（来自子元素的 ComputedStyle）。", static_cast<int>(justify_self));

    if (child.IsAnonymousBlockFlow()) {
      ipid_depth_log.FPrint(
          "子元素是匿名块流容器，设置 auto 宽度行为为 kStretchImplicit（隐式拉伸）。");
      builder.SetInlineAutoBehavior(AutoSizeBehavior::kStretchImplicit);
    } else if (justify_self == ItemPosition::kStretch) {
      ipid_depth_log.FPrint(
          "justify-self 为 stretch，设置 auto 宽度行为为 kStretchExplicit（显式拉伸）。");
      builder.SetInlineAutoBehavior(AutoSizeBehavior::kStretchExplicit);
    } else if (justify_self != ItemPosition::kNormal) {
      ipid_depth_log.FPrint(
          "justify-self 不是 normal（值为 {}），设置 auto 宽度行为为 kFitContent（适应内容）。",
          static_cast<int>(justify_self));
      builder.SetInlineAutoBehavior(AutoSizeBehavior::kFitContent);
    } else if (is_in_parallel_flow &&
               ShouldBlockContainerChildStretchAutoInlineSize(
                   To<BlockNode>(child))) {
      ipid_depth_log.FPrint(
          "writing-mode 平行且根据 CSS 规范子元素应拉伸，设置 auto 宽度行为为 kStretchImplicit（隐式拉伸）。");
      builder.SetInlineAutoBehavior(AutoSizeBehavior::kStretchImplicit);
    } else {
      ipid_depth_log.FPrint("子元素使用默认的 auto 宽度行为。");
    }
  }

  if (line_clamp_data_.ShouldHideForPaint()) [[unlikely]] {
    ipid_depth_log.FPrint(
        "由于 line-clamp 限制（line_clamp_data_.ShouldHideForPaint() 为 true），子元素在绘制时将被隐藏。");
    builder.SetIsHiddenForPaint(true);
  }

  ipid_depth_log.FPrint(
      "设置子元素的可用大小：{}（来自调用方传入的 child_available_size）。",
      ipid::GetLogicalSizeString(child_available_size));
  builder.SetAvailableSize(child_available_size);
  
  LogicalSize percentage_size = PercentageSizeForChild(child);
  ipid_depth_log.FPrint(
      "设置子元素的百分比解析大小：{}（通过 PercentageSizeForChild 计算得出）。",
      ipid::GetLogicalSizeString(percentage_size));
  builder.SetPercentageResolutionSize(percentage_size);

  // Pass the replaced %-size down to inline layout.
  if ((child.IsAnonymousBlockFlow() || child.IsInline()) &&
      replaced_child_percentage_size_ != child_percentage_size_) {
    ipid_depth_log.FPrint(
        "为匿名块流或内联元素传递替换元素的百分比大小：{}\n"
        "（来自成员变量 replaced_child_percentage_size_，与 child_percentage_size_ {} 不同）。",
        ipid::GetLogicalSizeString(replaced_child_percentage_size_),
        ipid::GetLogicalSizeString(child_percentage_size_));
    builder.SetReplacedChildPercentageResolutionSize(
        replaced_child_percentage_size_);
  }

  if (constraint_space.IsTableCell()) {
    ipid_depth_log.FPrint(
        "当前容器是表格单元格（从父约束空间 constraint_space.IsTableCell() 得知），设置子元素为表格单元格的子元素。");
    builder.SetIsTableCellChild(true);

    // Always shrink-to-fit children within a <mtd> element.
    if (Node().GetDOMNode() &&
        IsA<MathMLTableCellElement>(Node().GetDOMNode())) {
      ipid_depth_log.FPrint(
          "容器是 MathML 表格单元格（从 DOM 节点类型检测），强制子元素使用 fit-content 宽度行为。");
      builder.SetInlineAutoBehavior(AutoSizeBehavior::kFitContent);
    }

    // Some scrollable percentage-sized children of table-cells use their
    // min-size (instead of sizing normally).
    //
    // We only apply this rule if the block size of the containing table cell
    // is considered to be "restricted". Otherwise, especially if this is the
    // only child of the cell, and that is the only cell in the row, we'd end
    // up with zero block size.
    bool is_restricted_table_cell = constraint_space.IsRestrictedBlockSizeTableCell();
    bool child_has_indefinite_percentage_block_size = (child_percentage_size_.block_size == kIndefiniteSize);
    bool child_is_not_replaced = !child.ShouldBeConsideredAsReplaced();
    bool child_has_percent_height = child_style.LogicalHeight().HasPercent();
    bool child_has_scroll = (child_style.OverflowBlockDirection() == EOverflow::kAuto ||
                            child_style.OverflowBlockDirection() == EOverflow::kScroll);
    
    ipid_depth_log.FPrint(
        "检查是否应将子元素标记为受限高度表格单元格的子元素：\n"
        "- 表格单元格高度受限：{}\n"
        "- 子元素百分比块大小为不确定值：{}\n"
        "- 子元素不是替换元素：{}\n"
        "- 子元素有百分比高度：{}\n"
        "- 子元素有滚动行为：{}",
        ipid::btos(is_restricted_table_cell),
        ipid::btos(child_has_indefinite_percentage_block_size),
        ipid::btos(child_is_not_replaced),
        ipid::btos(child_has_percent_height),
        ipid::btos(child_has_scroll));
    
    if (is_restricted_table_cell && child_has_indefinite_percentage_block_size &&
        child_is_not_replaced && child_has_percent_height && child_has_scroll) {
      ipid_depth_log.FPrint(
          "所有条件满足，设置为受限高度表格单元格的子元素（这会影响其高度计算方式）。");
      builder.SetIsRestrictedBlockSizeTableCellChild();
    }
  }

  bool has_bfc_block_offset = container_builder_.BfcBlockOffset().has_value();
  ipid_depth_log.FPrint(
      "当前容器是否已确定 BFC 块偏移：{}（来自 container_builder_.BfcBlockOffset()）",
      ipid::btos(has_bfc_block_offset));

  // Propagate the |ConstraintSpace::ForcedBfcBlockOffset| down to our
  // children.
  if (!has_bfc_block_offset && constraint_space.ForcedBfcBlockOffset()) {
    LayoutUnit forced_offset = *constraint_space.ForcedBfcBlockOffset();
    ipid_depth_log.FPrint(
        "容器未确定 BFC 偏移且父约束空间有强制 BFC 偏移 {}（来自父级传递），传递给子元素。",
        forced_offset);
    builder.SetForcedBfcBlockOffset(forced_offset);
  }
  if (child_bfc_block_offset && !is_new_fc) {
    ipid_depth_log.FPrint(
        "子元素有指定 BFC 偏移 {} 且不是新格式化上下文（来自调用方传入的 child_bfc_block_offset），设置强制 BFC 偏移。",
        *child_bfc_block_offset);
    builder.SetForcedBfcBlockOffset(*child_bfc_block_offset);
  }

  if (has_bfc_block_offset) {
    ipid_depth_log.FPrint(
        "容器已确定 BFC 偏移，尝试为子元素设置乐观 BFC 偏移以提高布局缓存命中率。");
    // Typically we aren't allowed to look at the previous layout result within
    // a layout algorithm. However this is fine (honest), as it is just a hint
    // to the child algorithm for where floats should be placed. If it doesn't
    // have this flag, or gets this estimate wrong, it'll relayout with the
    // appropriate "forced" BFC block-offset.
    if (child.IsBlock()) {
      if (const LayoutResult* cached_result =
              child.GetLayoutBox()->GetCachedLayoutResult(
                  To<BlockBreakToken>(child_break_token))) {
        const auto& prev_space = cached_result->GetConstraintSpaceForCaching();
        ipid_depth_log.FPrint(
            "找到子元素的缓存布局结果，分析其 BFC 偏移变化来设置乐观偏移。");

        // To increase the hit-rate we adjust the previous "optimistic"/"forced"
        // BFC block-offset by how much the child has shifted from the previous
        // layout.
        LayoutUnit bfc_block_delta =
            child_data.bfc_offset_estimate.block_offset -
            prev_space.GetBfcOffset().block_offset;
        ipid_depth_log.FPrint(
            "计算 BFC 偏移变化量：当前预估 {} - 上次实际 {} = {}（来自 child_data 和缓存的 prev_space）",
            child_data.bfc_offset_estimate.block_offset,
            prev_space.GetBfcOffset().block_offset, bfc_block_delta);
        
        if (prev_space.ForcedBfcBlockOffset()) {
          LayoutUnit optimistic_offset = *prev_space.ForcedBfcBlockOffset() + bfc_block_delta;
          ipid_depth_log.FPrint(
              "基于上次强制 BFC 偏移 {} + 变化量 {} = {}，设置乐观 BFC 偏移。",
              *prev_space.ForcedBfcBlockOffset(), bfc_block_delta, optimistic_offset);
          builder.SetOptimisticBfcBlockOffset(optimistic_offset);
        } else if (prev_space.OptimisticBfcBlockOffset()) {
          LayoutUnit optimistic_offset = *prev_space.OptimisticBfcBlockOffset() + bfc_block_delta;
          ipid_depth_log.FPrint(
              "基于上次乐观 BFC 偏移 {} + 变化量 {} = {}，设置乐观 BFC 偏移。",
              *prev_space.OptimisticBfcBlockOffset(), bfc_block_delta, optimistic_offset);
          builder.SetOptimisticBfcBlockOffset(optimistic_offset);
        }
      } else {
        ipid_depth_log.FPrint("未找到子元素的缓存布局结果，无法设置乐观 BFC 偏移。");
      }
    }
  } else if (constraint_space.OptimisticBfcBlockOffset()) {
    // Propagate the |ConstraintSpace::OptimisticBfcBlockOffset| down to our
    // children.
    LayoutUnit optimistic_offset = *constraint_space.OptimisticBfcBlockOffset();
    ipid_depth_log.FPrint(
        "容器未确定 BFC 偏移但父约束空间有乐观 BFC 偏移 {}（来自父级），传递给子元素。",
        optimistic_offset);
    builder.SetOptimisticBfcBlockOffset(optimistic_offset);
  }

  // Propagate the |ConstraintSpace::AncestorHasClearancePastAdjoiningFloats|
  // flag down to our children.
  if (!has_bfc_block_offset &&
      constraint_space.AncestorHasClearancePastAdjoiningFloats()) {
    ipid_depth_log.FPrint(
        "容器未确定 BFC 偏移且祖先有跨越相邻浮动的 clearance（来自父约束空间），传递此标志给子元素。");
    builder.SetAncestorHasClearancePastAdjoiningFloats();
  }
  if (has_clearance_past_adjoining_floats) {
    ipid_depth_log.FPrint(
        "当前子元素有跨越相邻浮动的 clearance（来自调用方传入的 has_clearance_past_adjoining_floats），设置祖先 clearance 标志。");
    builder.SetAncestorHasClearancePastAdjoiningFloats();
  }

  LayoutUnit clearance_offset = LayoutUnit::Min();
  ipid_depth_log.FPrint("计算子元素的 clearance 偏移，初始值为最小值。");
  
  if (!IsBreakInside(DynamicTo<BlockBreakToken>(child_break_token))) {
    if (!constraint_space.IsNewFormattingContext()) {
      clearance_offset = constraint_space.ClearanceOffset();
      ipid_depth_log.FPrint(
          "从父约束空间继承 clearance 偏移：{}（因为父容器不是新格式化上下文）", clearance_offset);
    }
    if (child.IsBlock()) {
      EClear child_clear_value = child_style.Clear(Style());
      LayoutUnit child_clearance_offset =
          GetExclusionSpace().ClearanceOffset(child_clear_value);
      ipid_depth_log.FPrint(
          "根据子元素的 clear 属性 {} 计算的 clearance 偏移：{}（来自排斥空间和子元素样式）",
          ipid::GetComputedStyleEClearStr(child_clear_value), child_clearance_offset);
      clearance_offset = std::max(clearance_offset, child_clearance_offset);
      ipid_depth_log.FPrint(
          "最终的 clearance 偏移（取较大值）：{}", clearance_offset);
    }
  } else {
    ipid_depth_log.FPrint("子元素在分割内部，不设置 clearance 偏移。");
  }
  
  builder.SetClearanceOffset(clearance_offset);
  builder.SetBaselineAlgorithmType(constraint_space.GetBaselineAlgorithmType());
  ipid_depth_log.FPrint(
      "设置基线算法类型：{}（来自父约束空间）",
      static_cast<int>(constraint_space.GetBaselineAlgorithmType()));

  if (child_data.is_pushed_by_floats) {
    // Clearance has been applied, but it won't be automatically detected when
    // laying out the child, since the BFC block-offset has already been updated
    // to be past the relevant floats. We therefore need a flag.
    ipid_depth_log.FPrint(
        "子元素被浮动推移了位置（来自 child_data.is_pushed_by_floats），设置相应标志以便子元素布局时正确处理。");
    builder.SetIsPushedByFloats();
  }

  if (!is_new_fc) {
    ipid_depth_log.FPrint(
        "子元素不是新的格式化上下文，需要继承父容器的上下文信息。\n"
        "设置 margin strut: {}（来自 child_data）\n"
        "设置 BFC 偏移预估: {}（来自 child_data）\n"
        "传递排斥空间（浮动信息）。",
        ipid::GetMarginStrutString(child_data.margin_strut),
        child_data.bfc_offset_estimate);
    
    builder.SetMarginStrut(child_data.margin_strut);
    builder.SetBfcOffset(child_data.bfc_offset_estimate);
    builder.SetExclusionSpace(GetExclusionSpace());
    
    if (!has_bfc_block_offset) {
      ipid_depth_log.FPrint(
          "容器未确定 BFC 偏移，传递相邻对象类型信息（来自 container_builder_）。");
      builder.SetAdjoiningObjectTypes(
          container_builder_.GetAdjoiningObjectTypes());
    }
    
    bool should_text_box_trim = line_clamp_data_.data.IsLineClampContext() &&
        (constraint_space.ShouldTextBoxTrimInsideWhenLineClamp() ||
         container_builder_.ShouldTextBoxTrimNodeEnd());
    ipid_depth_log.FPrint(
        "设置 line-clamp 相关数据和文本框裁剪设置。\n"
        "- line-clamp 数据：来自 line_clamp_data_.data\n"
        "- 结束 margin strut：{}（来自 line_clamp_data_）\n"
        "- 结束 padding：{}（来自当前容器的 padding）\n"
        "- 是否在 line-clamp 时进行内部文本框裁剪：{}",
        ipid::GetMarginStrutString(line_clamp_data_.end_margin_strut),
        Padding().block_end, ipid::btos(should_text_box_trim));
    
    builder.SetLineClampData(line_clamp_data_.data);
    builder.SetLineClampEndMarginStrut(line_clamp_data_.end_margin_strut);
    builder.SetLineClampEndPadding(Padding().block_end);
    builder.SetShouldTextBoxTrimInsideWhenLineClamp(should_text_box_trim);
  } else {
    ipid_depth_log.FPrint(
        "子元素是新的格式化上下文，不继承父容器的 margin strut 等上下文信息。");
  }
  if (block_start_annotation_space != LayoutUnit()) {
    ipid_depth_log.FPrint(
        "设置块开始注释空间：{}（来自调用方传入）", block_start_annotation_space);
  }
  builder.SetBlockStartAnnotationSpace(block_start_annotation_space);

  // Propagate `text-box-trim` only for in-flow children.
  if (container_builder_.ShouldTextBoxTrim() &&
      !child.IsFloatingOrOutOfFlowPositioned()) [[unlikely]] {
    ipid_depth_log.FPrint(
        "容器需要文本框裁剪（来自 container_builder_.ShouldTextBoxTrim()）且子元素是流内元素，配置文本框裁剪参数。");
    
    // For inline children we cannot determine here whether this will be the
    // last line or not. However, `InlineLayoutAlgorithm` can determine if it's
    // the last line or not rather quickly in most cases. If it fails to apply
    // end trimming when it should (happens for block-in-inline), this is
    // handled by `RelayoutForTextBoxTrimEnd()`.
    bool known_to_have_successive_content =
        !child.IsInline() && !IsLastInflowChild(*child.GetLayoutBox());
    
    ipid_depth_log.FPrint(
        "子元素是否确定有后续内容：{}（基于子元素类型和在父容器中的位置判断）",
        ipid::btos(known_to_have_successive_content));

    SetTextBoxTrimOnChildSpaceBuilder(
        container_builder_, known_to_have_successive_content, &builder);
    if (container_builder_.ShouldTextBoxTrimEnd()) {
      bool should_force_trim_end = child.IsInline() && 
          child == override_text_box_trim_end_child_ &&
          InlineBreakToken::IsStartEqual(
              To<InlineBreakToken>(override_text_box_trim_end_break_token_),
              To<InlineBreakToken>(child_break_token));
      if (should_force_trim_end) {
        ipid_depth_log.FPrint(
            "内联子元素匹配文本框裁剪结束的覆盖设置（来自成员变量比较），强制启用结束裁剪。");
        builder.SetShouldForceTextBoxTrimEnd();
      }
    }
  }

  if (constraint_space.HasBlockFragmentation()) {
    ipid_depth_log.FPrint(
        "父约束空间有块分片（分页/分列），配置子元素的分片相关参数。");
    
    LayoutUnit fragmentainer_offset_delta;
    // We need to keep track of our block-offset within the fragmentation
    // context, to be able to tell where the fragmentation line is (i.e. where
    // to break).
    if (is_new_fc) {
      fragmentainer_offset_delta =
          *child_bfc_block_offset - constraint_space.ExpectedBfcBlockOffset();
      ipid_depth_log.FPrint(
          "子元素是新格式化上下文，计算分片器偏移差值：{} - {} = {}（来自参数和父约束空间）",
          *child_bfc_block_offset, constraint_space.ExpectedBfcBlockOffset(),
          fragmentainer_offset_delta);
    } else {
      fragmentainer_offset_delta = builder.ExpectedBfcBlockOffset() -
                                   constraint_space.ExpectedBfcBlockOffset();
      ipid_depth_log.FPrint(
          "子元素不是新格式化上下文，计算分片器偏移差值：{} - {} = {}（来自构建器和父约束空间）",
          builder.ExpectedBfcBlockOffset(), constraint_space.ExpectedBfcBlockOffset(),
          fragmentainer_offset_delta);
    }
    
    SetupSpaceBuilderForFragmentation(container_builder_, child,
                                      fragmentainer_offset_delta, &builder);

    if (!is_new_fc && GetConstraintSpace().IsInColumnBfc()) {
      // Need to keep track of whether we're in the same formatting context as a
      // column, in order to determine whether column-span:all applies on a
      // descendant.
      ipid_depth_log.FPrint(
          "子元素不是新格式化上下文且在列 BFC 中（来自父约束空间），设置相应标志以支持 column-span:all 检测。");
      builder.SetIsInColumnBfc();
    }

    // If there's a child break inside (typically in a parallel flow, or we
    // would have finished layout by now), we need to produce more
    // fragmentainers, before we can insert any column spanners, so that
    // everything that is supposed to come before the spanner actually ends up
    // there.
    bool is_past_break = constraint_space.IsPastBreak();
    bool has_inserted_child_break = container_builder_.HasInsertedChildBreak();
    if (is_past_break || has_inserted_child_break) {
      ipid_depth_log.FPrint(
          "存在跨分片的中断（父约束空间：{}）或容器已插入子元素中断（container_builder_：{}），设置跨分片标志。",
          ipid::btos(is_past_break), ipid::btos(has_inserted_child_break));
      builder.SetIsPastBreak();
    }
  }

  const bool has_stretch =
      IsHorizontalWritingMode(constraint_space.GetWritingMode())
          ? child_style.Height().HasStretch() ||
                child_style.MinHeight().HasStretch() ||
                child_style.MaxHeight().HasStretch()
          : child_style.Width().HasStretch() ||
                child_style.MinWidth().HasStretch() ||
                child_style.MaxWidth().HasStretch();
  
  ipid_depth_log.FPrint(
      "检查子元素是否在块方向上有 stretch 尺寸：{}（基于 writing-mode {} 和子元素样式检测）",
      ipid::btos(has_stretch), static_cast<int>(constraint_space.GetWritingMode()));

  if (has_stretch && !constraint_space.IsNewFormattingContext()) {
    ipid_depth_log.FPrint(
        "子元素有 stretch 尺寸且父容器不是新格式化上下文，配置 stretch 时忽略 margin 的设置。");
    
    bool ignore_block_start = BorderPadding().block_start == LayoutUnit();
    bool ignore_block_end = BorderPadding().block_end == LayoutUnit();
    ipid_depth_log.FPrint(
        "根据容器的 border+padding 决定忽略 margin 的边：block_start={}, block_end={}（来自当前容器的 BorderPadding()）",
        ipid::btos(ignore_block_start), ipid::btos(ignore_block_end));
    
    const LineLogicalBoxSides sides(ignore_block_start,
                                    /* line_right */ false,
                                    ignore_block_end,
                                    /* line_left */ false);
    builder.SetIgnoreMarginsForStretch(constraint_space.GetWritingMode(),
                                       sides);
  }

  ConstraintSpace result = builder.ToConstraintSpace();
  ipid_depth_log.FPrint(
      "完成子元素约束空间的构建。最终约束空间：{}",
      ipid::GetConstraintSpaceString(result));
  
  return result;
}

void BlockLayoutAlgorithm::PropagateBaselineFromLineBox(
    const PhysicalFragment& child,
    LayoutUnit block_offset) {
  const auto& line_box = To<PhysicalLineBoxFragment>(child);

  // Skip over a line-box which is empty. These don't have any baselines
  // which should be added.
  if (line_box.IsEmptyLineBox())
    return;

  // Skip over the line-box if we are past our clamp point.
  if (line_clamp_data_.IsPastClampPoint()) {
    return;
  }

  if (line_box.IsBlockInInline()) [[unlikely]] {
    // Block-in-inline may have different first/last baselines.
    DCHECK(container_builder_.ItemsBuilder());
    const auto& items =
        container_builder_.ItemsBuilder()->GetLogicalLineItems(line_box);
    const LayoutResult* result = items.BlockInInlineLayoutResult();
    DCHECK(result);
    PropagateBaselineFromBlockChild(result->GetPhysicalFragment(),
                                    /* margins */ BoxStrut(), block_offset);
    return;
  }

  FontHeight metrics = line_box.BaselineMetrics();
  DCHECK(!metrics.IsEmpty());
  LayoutUnit baseline =
      block_offset +
      (Style().IsFlippedLinesWritingMode() ? metrics.descent : metrics.ascent);

  if (!container_builder_.FirstBaseline())
    container_builder_.SetFirstBaseline(baseline);
  container_builder_.SetLastBaseline(baseline);
}

void BlockLayoutAlgorithm::PropagateBaselineFromBlockChild(
    const PhysicalFragment& child,
    const BoxStrut& margins,
    LayoutUnit block_offset) {
  DCHECK(child.IsBox());
  const auto baseline_algorithm =
      GetConstraintSpace().GetBaselineAlgorithmType();

  // When computing baselines for an inline-block, table's don't contribute any
  // baselines.
  if (child.IsTable() &&
      baseline_algorithm == BaselineAlgorithmType::kInlineBlock) {
    return;
  }

  // Skip over the block if we are past our clamp point.
  if (line_clamp_data_.IsPastClampPoint()) {
    return;
  }

  const auto& physical_fragment = To<PhysicalBoxFragment>(child);
  LogicalBoxFragment fragment(GetConstraintSpace().GetWritingDirection(),
                              physical_fragment);

  if (!container_builder_.FirstBaseline()) {
    if (auto first_baseline = fragment.FirstBaseline())
      container_builder_.SetFirstBaseline(block_offset + *first_baseline);
  }

  // Counter-intuitively, when computing baselines for an inline-block, some
  // fragments use their first-baseline for the container's last-baseline.
  bool use_last_baseline =
      baseline_algorithm == BaselineAlgorithmType::kDefault ||
      physical_fragment.UseLastBaselineForInlineBaseline();

  auto last_baseline =
      use_last_baseline ? fragment.LastBaseline() : fragment.FirstBaseline();

  // When computing baselines for an inline-block, some block-boxes (e.g. with
  // "overflow: hidden") will force the baseline to the block-end margin edge.
  if (baseline_algorithm == BaselineAlgorithmType::kInlineBlock &&
      physical_fragment.ForceInlineBaselineSynthesis() &&
      fragment.IsWritingModeEqual()) {
    last_baseline = fragment.BlockSize() + margins.block_end;
  }

  if (last_baseline)
    container_builder_.SetLastBaseline(block_offset + *last_baseline);
}

bool BlockLayoutAlgorithm::ResolveBfcBlockOffset(
    PreviousInflowPosition* previous_inflow_position,
    LayoutUnit bfc_block_offset,
    std::optional<LayoutUnit> forced_bfc_block_offset) {
  // Clearance may have been resolved (along with BFC block-offset) in a
  // previous layout pass, so check the constraint space for pre-applied
  // clearance. This is important in order to identify possible class C break
  // points.
  if (GetConstraintSpace().IsPushedByFloats()) {
    container_builder_.SetIsPushedByFloats();
  }

  if (container_builder_.BfcBlockOffset())
    return true;

  bfc_block_offset = forced_bfc_block_offset.value_or(bfc_block_offset);

  if (ApplyClearance(GetConstraintSpace(), &bfc_block_offset)) {
    container_builder_.SetIsPushedByFloats();
  }

  container_builder_.SetBfcBlockOffset(bfc_block_offset);

  if (NeedsAbortOnBfcBlockOffsetChange()) {
    // A formatting context root should always be able to resolve its
    // whereabouts before layout, so there should never be any incorrect
    // estimates that we need to go back and fix.
    DCHECK(!GetConstraintSpace().IsNewFormattingContext());

    return false;
  }

  // Set the offset to our block-start border edge. We'll now end up at the
  // block-start border edge. If the BFC block offset was resolved due to a
  // block-start border or padding, that must be added by the caller, for
  // subsequent layout to continue at the right position. Whether we need to add
  // border+padding or not isn't something we should determine here, so it must
  // be dealt with as part of initializing the layout algorithm.
  previous_inflow_position->logical_block_offset = LayoutUnit();

  // Resolving the BFC offset normally means that we have finished collapsing
  // adjoining margins, so that we can reset the margin strut. One exception
  // here is if we're resuming after a break, in which case we know that we can
  // resolve the BFC offset to the block-start of the fragmentainer
  // (block-offset 0). But keep the margin strut, since we're essentially still
  // collapsing with the fragmentainer boundary, which will eat / discard all
  // adjoining margins - unless this is at a forced break. DCHECK that the strut
  // is empty (note that a strut that's set up to eat all margins will also be
  // considered to be empty).
  if (!is_resuming_)
    previous_inflow_position->margin_strut = MarginStrut();
  else
    DCHECK(previous_inflow_position->margin_strut.IsEmpty());

  return true;
}

bool BlockLayoutAlgorithm::NeedsAbortOnBfcBlockOffsetChange() const {
  DCHECK(container_builder_.BfcBlockOffset());
  if (!abort_when_bfc_block_offset_updated_)
    return false;

  // If our position differs from our (potentially optimistic) estimate, abort.
  return *container_builder_.BfcBlockOffset() !=
         GetConstraintSpace().ExpectedBfcBlockOffset();
}

std::optional<LayoutUnit>
BlockLayoutAlgorithm::CalculateQuirkyBodyMarginBlockSum(
    const MarginStrut& end_margin_strut) {
  if (!Node().IsQuirkyAndFillsViewport())
    return std::nullopt;

  if (!Style().LogicalHeight().IsAuto()) {
    return std::nullopt;
  }

  if (GetConstraintSpace().IsNewFormattingContext()) {
    return std::nullopt;
  }

  DCHECK(Node().IsBody());
  LayoutUnit block_end_margin =
      ComputeMarginsForSelf(GetConstraintSpace(), Style()).block_end;

  // The |end_margin_strut| is the block-start margin if the body doesn't have
  // a resolved BFC block-offset.
  if (!container_builder_.BfcBlockOffset())
    return end_margin_strut.Sum() + block_end_margin;

  MarginStrut body_strut = end_margin_strut;
  body_strut.Append(block_end_margin, Style().HasMarginBlockEndQuirk());
  return *container_builder_.BfcBlockOffset() -
         GetConstraintSpace().GetBfcOffset().block_offset + body_strut.Sum();
}

bool BlockLayoutAlgorithm::PositionOrPropagateListMarker(
    const LayoutResult& layout_result,
    LogicalOffset* content_offset,
    PreviousInflowPosition* previous_inflow_position) {
  // If this is not a list-item, propagate unpositioned list markers to
  // ancestors.
  if (!ShouldPlaceUnpositionedListMarker())
    return true;

  // If this is a list item, add the unpositioned list marker as a child.
  UnpositionedListMarker list_marker =
      container_builder_.GetUnpositionedListMarker();
  if (!list_marker)
    return true;
  container_builder_.ClearUnpositionedListMarker();

  const ConstraintSpace& space = GetConstraintSpace();
  const auto& content = layout_result.GetPhysicalFragment();
  FontBaseline baseline_type = Style().GetFontBaseline();
  if (auto content_baseline =
          list_marker.ContentAlignmentBaseline(space, baseline_type, content)) {
    // TODO: We are reusing the ConstraintSpace for LI here. It works well for
    // now because authors cannot style list-markers currently. If we want to
    // support `::marker` pseudo, we need to create ConstraintSpace for marker
    // separately.
    const LayoutResult* marker_layout_result =
        list_marker.Layout(space, container_builder_.Style(), baseline_type);
    DCHECK(marker_layout_result);
    // If the BFC block-offset of li is still not resolved, resolved it now.
    if (!container_builder_.BfcBlockOffset() &&
        marker_layout_result->BfcBlockOffset()) {
      // TODO: Currently the margin-top of marker is always zero. To support
      // `::marker` pseudo, we should count marker's margin-top in.
#if DCHECK_IS_ON()
      list_marker.CheckMargin();
#endif
      if (!ResolveBfcBlockOffset(previous_inflow_position))
        return false;
    }

    list_marker.AddToBox(space, baseline_type, content,
                         BorderScrollbarPadding(), *marker_layout_result,
                         *content_baseline, &content_offset->block_offset,
                         &container_builder_);
    return true;
  }

  // If the list marker could not be positioned against this child because it
  // does not have the baseline to align to, keep it as unpositioned and try
  // the next child.
  container_builder_.SetUnpositionedListMarker(list_marker);
  return true;
}

bool BlockLayoutAlgorithm::PositionListMarkerWithoutLineBoxes(
    PreviousInflowPosition* previous_inflow_position) {
  DCHECK(ShouldPlaceUnpositionedListMarker());
  DCHECK(container_builder_.GetUnpositionedListMarker());

  auto list_marker = container_builder_.GetUnpositionedListMarker();
  const ConstraintSpace& space = GetConstraintSpace();
  FontBaseline baseline_type = Style().GetFontBaseline();
  // Layout the list marker.
  const LayoutResult* marker_layout_result =
      list_marker.Layout(space, container_builder_.Style(), baseline_type);
  DCHECK(marker_layout_result);
  // If the BFC block-offset of li is still not resolved, resolve it now.
  if (!container_builder_.BfcBlockOffset() &&
      marker_layout_result->BfcBlockOffset()) {
    // TODO: Currently the margin-top of marker is always zero. To support
    // `::marker` pseudo, we should count marker's margin-top in.
#if DCHECK_IS_ON()
    list_marker.CheckMargin();
#endif
    if (!ResolveBfcBlockOffset(previous_inflow_position))
      return false;
  }
  // Position the list marker without aligning to line boxes.
  list_marker.AddToBoxWithoutLineBoxes(
      space, baseline_type, *marker_layout_result, &container_builder_,
      &intrinsic_block_size_);
  container_builder_.ClearUnpositionedListMarker();

  return true;
}

BlockLayoutAlgorithm::PlaceholderLayoutResult
BlockLayoutAlgorithm::HandleTextControlPlaceholder(
    BlockNode placeholder,
    const PreviousInflowPosition& previous_inflow_position) {
  DCHECK(Node().IsTextControl()) << Node().GetLayoutBox();

  const wtf_size_t kTextBlockIndex = 0u;
  LogicalSize available_size = ChildAvailableSize();
  bool apply_fixed_size = Style().ApplyControlFixedSize(Node().GetDOMNode());
  if (container_builder_.Children().size() > 0 && apply_fixed_size) {
    // The placeholder should have the width same as "editing-view-port"
    // element, which is the first grandchild of the text control.
    const PhysicalFragment& child =
        *container_builder_.Children()[kTextBlockIndex].fragment;
    if (child.IsTextControlContainer()) {
      const auto& grand_children =
          To<PhysicalBoxFragment>(child).PostLayoutChildren();
      const auto begin = grand_children.begin();
      if (begin != grand_children.end()) {
        LogicalFragment grand_child_fragment(
            GetConstraintSpace().GetWritingDirection(), *begin->fragment);
        available_size.inline_size = grand_child_fragment.InlineSize();
      }
    }
  }

  const bool is_new_fc = placeholder.CreatesNewFormattingContext();
  const InflowChildData child_data =
      ComputeChildData(previous_inflow_position, placeholder,
                       /* child_break_token */ nullptr, is_new_fc);
  const ConstraintSpace space = CreateConstraintSpaceForChild(
      placeholder, /* child_break_token */ nullptr, child_data, available_size,
      is_new_fc);

  const LayoutResult* result = placeholder.Layout(space);
  // crbug.com/439682405 and crbug.com/440833172: The placeholder layout may
  // fail.
  if (RuntimeEnabledFeatures::AbortByPlaceholderLayoutEnabled() &&
      result->Status() != LayoutResult::kSuccess) {
    return {previous_inflow_position.logical_block_offset, result->Status()};
  }
  LogicalOffset offset = BorderScrollbarPadding().StartOffset();
  if (Node().IsTextArea()) {
    return {FinishTextControlPlaceholder(result, offset, apply_fixed_size,
                                         previous_inflow_position),
            result->Status()};
  }
  // Usually another child provides the baseline. However it doesn't if
  // another child is out-of-flow.
  if (!container_builder_.FirstBaseline()) {
    return {FinishTextControlPlaceholder(result, offset, apply_fixed_size,
                                         previous_inflow_position),
            result->Status()};
  }
  LogicalBoxFragment fragment(
      GetConstraintSpace().GetWritingDirection(),
      To<PhysicalBoxFragment>(result->GetPhysicalFragment()));
  // We should apply FirstBaseline() of the placeholder fragment because the
  // placeholder might have the 'overflow' property, and its LastBaseline()
  // might be the block-end margin.
  // |fragment| has no FirstBaseline() if it consists of only white-spaces.
  if (fragment.FirstBaseline().has_value()) {
    LayoutUnit first_baseline = *container_builder_.FirstBaseline();
    const LayoutUnit border_padding_block_start =
        BorderScrollbarPadding().block_start;
    const LayoutUnit placeholder_baseline = *fragment.FirstBaseline();
    offset.block_offset = first_baseline - placeholder_baseline;
    if (!apply_fixed_size && offset.block_offset < border_padding_block_start) {
      // The placeholder is taller. We should shift down the existing child.
      const LayoutUnit new_baseline =
          placeholder_baseline + border_padding_block_start;
      container_builder_.SetFirstBaseline(new_baseline);
      container_builder_.SetLastBaseline(new_baseline);
      const LogicalFragmentLink& first_child =
          container_builder_.Children()[kTextBlockIndex];
      LogicalOffset first_child_offset = first_child.offset;
      first_child_offset.block_offset += new_baseline - first_baseline;
      container_builder_.ReplaceChild(kTextBlockIndex, *first_child.fragment,
                                      first_child_offset);
      offset.block_offset = border_padding_block_start;
    }
  }
  return {FinishTextControlPlaceholder(result, offset, apply_fixed_size,
                                       previous_inflow_position),
          result->Status()};
}

LayoutUnit BlockLayoutAlgorithm::FinishTextControlPlaceholder(
    const LayoutResult* result,
    const LogicalOffset& offset,
    bool apply_fixed_size,
    const PreviousInflowPosition& previous_inflow_position) {
  container_builder_.AddResult(*result, offset);
  LayoutUnit block_offset = previous_inflow_position.logical_block_offset;
  if (apply_fixed_size) {
    return block_offset;
  }
  LogicalBoxFragment fragment(
      GetConstraintSpace().GetWritingDirection(),
      To<PhysicalBoxFragment>(result->GetPhysicalFragment()));
  return std::max(block_offset, offset.block_offset + fragment.BlockSize());
}

LogicalOffset BlockLayoutAlgorithm::AdjustSliderThumbInlineOffset(
    const LogicalFragment& fragment,
    const LogicalOffset& logical_offset) {
  const LayoutUnit available_extent =
      ChildAvailableSize().inline_size - fragment.InlineSize();
  const auto* input =
      To<HTMLInputElement>(Node().GetDOMNode()->OwnerShadowHost());
  LayoutUnit offset(input->RatioValue().ToDouble() * available_extent);
  return {logical_offset.inline_offset + offset, logical_offset.block_offset};
}

void BlockLineClampData::UpdateFromStyle(int lines_until_clamp,
                                         LayoutUnit clamp_bfc_offset) {
  if (ignore_line_clamp) {
    DCHECK_EQ(data.state, LineClampData::kDisabled);
    return;
  }

  DCHECK_EQ(data.state, LineClampData::kDisabled);
  DCHECK_GE(lines_until_clamp, 0);
  if (!lines_until_clamp) {
    if (clamp_bfc_offset == kIndefiniteSize) {
      data.state = LineClampData::kDisabled;
    } else {
      data.state = LineClampData::kMeasureLinesUntilBfcOffset;
      data.lines_until_clamp = 0;
      data.clamp_bfc_offset = clamp_bfc_offset;
    }
  } else {
    if (clamp_bfc_offset == kIndefiniteSize) {
      data.state = LineClampData::kClampByLines;
      data.lines_until_clamp = lines_until_clamp;
    } else {
      data.state = LineClampData::kClampByLinesWithBfcOffset;
      data.lines_until_clamp = lines_until_clamp;
      data.clamp_bfc_offset = clamp_bfc_offset;
    }
  }
}

bool BlockLineClampData::UpdateAfterLayout(
    const LayoutResult* layout_result,
    Document& document,
    LayoutUnit bfc_block_offset,
    const PreviousInflowPosition& previous_inflow_position,
    LayoutUnit block_end_padding) {
  const PhysicalFragment& fragment = layout_result->GetPhysicalFragment();

  int old_lines_until_clamp = 0;
  if (data.IsClampByLines() || data.IsMeasureUntilBfcOffset()) {
    old_lines_until_clamp = data.lines_until_clamp;
    if (!fragment.IsFormattingContextRoot() && !ignore_further_lines) {
      data.lines_until_clamp = layout_result->LinesUntilClamp();
    }
  }

  if (data.IsMeasureUntilBfcOffset() &&
      !previous_inflow_position_when_clamped.has_value()) {
    // We compute the margin strut we'd have after this block if we were to
    // clamp here.
    MarginStrut collapsed_strut = previous_inflow_position.margin_strut;
    collapsed_strut.positive_margin = std::max(
        collapsed_strut.positive_margin, end_margin_strut.positive_margin);
    collapsed_strut.quirky_positive_margin =
        std::max(collapsed_strut.quirky_positive_margin,
                 end_margin_strut.quirky_positive_margin);
    collapsed_strut.negative_margin = std::max(
        collapsed_strut.negative_margin, end_margin_strut.negative_margin);

    // The extra space after the current box that would be added by ruby
    // annotations, considering that the annotations eat into the following
    // padding if it exists, and that we have already subtracted the block end
    // padding from the clamp BFC offset.
    LayoutUnit padding_annotation_overflow;
    if (previous_inflow_position.block_end_annotation_space < LayoutUnit()) {
      padding_annotation_overflow =
          std::max(previous_inflow_position.block_end_annotation_space,
                   -block_end_padding);
    }

    LayoutUnit bfc_offset = bfc_block_offset +
                            previous_inflow_position.logical_block_offset +
                            padding_annotation_overflow +
                            (collapsed_strut.Sum() - end_margin_strut.Sum());

    if (bfc_offset > data.clamp_bfc_offset) {
      if (data.IsClampByLines()) {
        UseCounter::Count(document, WebFeature::kLineClampByLinesOverflows);
      }
      if (RuntimeEnabledFeatures::CSSLineClampEnabled()) {
        data.lines_until_clamp = old_lines_until_clamp;
        return false;
      }
    }

    if (old_lines_until_clamp == data.lines_until_clamp ||
        layout_result->LineClampAfterLayoutObject()) {
      // Empty line boxes should be ignored, they shouldn't even set
      // last_layout_object to null. Other fragments shouldn't have a null
      // layout object.
      if (!fragment.IsLineBox()) {
        last_layout_object = fragment.GetLayoutObject();
        DCHECK(last_layout_object);
      }
    } else {
      last_layout_object = nullptr;
    }
  }

  if (data.IsClampByLines()) {
    if (layout_result->WouldBeLastLineIfNotForEllipsis()) {
      DCHECK(fragment.IsLineBox());
      DCHECK_EQ(data.lines_until_clamp, 0);
      ignore_further_lines = true;
    }

    if (IsPastClampPoint() &&
        !previous_inflow_position_when_clamped.has_value()) {
      previous_inflow_position_when_clamped = previous_inflow_position;
    }
  }

  // With kClampAfterLayoutObject, if we've found the layout object, then we
  // switch states to kClampByLines with negative lines. If the child layout
  // result has negative lines, then the layout object was found there.
  if (data.state == LineClampData::kClampAfterLayoutObject &&
      (layout_result->LinesUntilClamp() < 0 ||
       data.clamp_after_layout_object == fragment.GetLayoutObject())) {
    DCHECK(!previous_inflow_position_when_clamped);
    data.state = LineClampData::kClampByLines;
    data.lines_until_clamp = -1;
    previous_inflow_position_when_clamped = previous_inflow_position;
  }

  return true;
}

}  // namespace blink
