#include "third_party/blink/renderer/core/layout/ipid_debug_layout_str_utils.h"

#include <format>
#include <sstream>

#include "third_party/blink/renderer/core/layout/baseline_utils.h"
#include "third_party/blink/renderer/core/layout/flex/flex_item.h"
#include "third_party/blink/renderer/core/layout/flex/flex_layout_algorithm.h"
#include "third_party/blink/renderer/core/layout/flex/flex_line.h"
#include "third_party/blink/renderer/core/layout/flex/line_flexer.h"
#include "third_party/blink/renderer/core/layout/fragmentation_utils.h"
#include "third_party/blink/renderer/core/layout/geometry/box_strut.h"
#include "third_party/blink/renderer/core/layout/geometry/logical_size.h"
#include "third_party/blink/renderer/core/layout/geometry/margin_strut.h"
#include "third_party/blink/renderer/core/layout/inline/inline_node.h"
#include "third_party/blink/renderer/core/layout/inline/line_breaker.h"
#include "third_party/blink/renderer/core/layout/ipid_debug_layout_str_utils.h"
#include "third_party/blink/renderer/core/layout/layout_object.h"
#include "third_party/blink/renderer/core/layout/layout_result.h"
#include "third_party/blink/renderer/core/layout/layout_utils.h"
#include "third_party/blink/renderer/core/layout/length_utils.h"
#include "third_party/blink/renderer/core/layout/min_max_sizes.h"
#include "third_party/blink/renderer/core/layout/physical_fragment.h"
#include "third_party/blink/renderer/core/style/computed_style_base_constants.h"
#include "third_party/blink/renderer/core/style/computed_style_constants.h"
#include "third_party/blink/renderer/core/style/style_content_alignment_data.h"

namespace blink::ipid {

const char* btos(const bool b) {
  return b ? "true" : "false";
}

// 返回 SizeType 的字符串表示和业务逻辑
const char* GetSizeTypeString(const SizeType type) {
  switch (type) {
    case SizeType::kContent:
      return "kContent (考虑 aspect-ratio 影响)";
    case SizeType::kIntrinsic:
      return "kIntrinsic (排除 aspect-ratio 影响)";
    default:
      return "<Unknown>";
  }
}

// 返回LayoutResult::EStatus的字符串表示
const char* GetLayoutResultStatusString(const int status) {
  switch (static_cast<LayoutResult::EStatus>(status)) {
    case LayoutResult::EStatus::kSuccess:
      return "kSuccess";
    case LayoutResult::EStatus::kBfcBlockOffsetResolved:
      return "kBfcBlockOffsetResolved";
    case LayoutResult::EStatus::kNeedsEarlierBreak:
      return "kNeedsEarlierBreak";
    case LayoutResult::EStatus::kOutOfFragmentainerSpace:
      return "kOutOfFragmentainerSpace";
    case LayoutResult::EStatus::kNeedsLineClampRelayout:
      return "kNeedsLineClampRelayout";
    case LayoutResult::EStatus::kDisableFragmentation:
      return "kDisableFragmentation";
    case LayoutResult::EStatus::kNeedsRelayoutWithNoChildScrollbarChanges:
      return "kNeedsRelayoutWithNoChildScrollbarChanges";
    case LayoutResult::EStatus::kTextBoxTrimEndDidNotApply:
      return "kTextBoxTrimEndDidNotApply";
    case LayoutResult::EStatus::kAlgorithmSpecific1:
      return "kAlgorithmSpecific1/kNeedsRelayoutWithRowCrossSizeChanges/"
             "kNeedsRelayoutAsLastTableBox";
    default:
      return "<Unknown>";
  }
}

std::string GetMinMaxSizesString(const MinMaxSizes& sizes) {
  std::ostringstream oss;
  oss << sizes;
  return oss.str();
}

std::string GetMinMaxSizesResultString(const MinMaxSizesResult& result) {
  std::ostringstream oss;
  oss << "MinMaxSizesResult" << result.sizes;
  return oss.str();
}

std::string GetAspectRatioString(const LogicalSize& aspect_ratio) {
  std::ostringstream oss;
  oss << aspect_ratio.inline_size << " / " << aspect_ratio.block_size;
  return oss.str();
}

std::string GetLayoutResultString(const LayoutResult* layout_result) {
  if (!layout_result) {
    return "LayoutResult { nullptr }";
  }

  std::ostringstream oss;

  oss << "LayoutResult { ";
  LayoutResult::EStatus status = layout_result->Status();

  if (status == LayoutResult::EStatus::kSuccess) {
    oss << "根元素大小: "
        << layout_result->GetPhysicalFragment().Size().ToString().Utf8();
  }

  oss << " }";
  return oss.str();
}

// 返回AutoSizeBehavior的字符串表示
const char* GetAutoSizeBehaviorString(const AutoSizeBehavior& behavior) {
  switch (behavior) {
    case AutoSizeBehavior::kFitContent:
      return "kFitContent";
    case AutoSizeBehavior::kStretchImplicit:
      return "kStretchImplicit";
    case AutoSizeBehavior::kStretchExplicit:
      return "kStretchExplicit";
    default:
      return "<Unknown>";
  }
}

std::string GetConstraintSpaceString(const ConstraintSpace& constraint_space) {
  std::ostringstream oss;

  oss << "ConstraintSpace {"
      << "\n  available_size = (" << constraint_space.AvailableSize() << "),";

  if (constraint_space.PercentageResolutionSize() !=
      constraint_space.AvailableSize()) {
    oss << "  percentage_resolution_size = ("
        << constraint_space.PercentageResolutionSize() << "),";
  }

  oss << "\n  inline_auto_behavior = "
      << GetAutoSizeBehaviorString(constraint_space.InlineAutoBehavior()) << ","
      << "\n  block_auto_behavior = "
      << GetAutoSizeBehaviorString(constraint_space.BlockAutoBehavior()) << ","
      << "\n}";

  return oss.str();
}

// 返回LengthTypeInternal的字符串表示
const char* GetLengthTypeInternalString(const LengthTypeInternal& type) {
  switch (type) {
    case LengthTypeInternal::kMin:
      return "kMin";
    case LengthTypeInternal::kMain:
      return "kMain";
    case LengthTypeInternal::kMax:
      return "kMax";
    default:
      return "<Unknown>";
  }
}

// 返回FitContentMode的字符串表示
const char* GetFitContentModeString(const FitContentMode& mode) {
  switch (mode) {
    case FitContentMode::kNormal:
      return "kNormal";
    case FitContentMode::kMinContribution:
      return "kMinContribution";
    case FitContentMode::kMaxContribution:
      return "kMaxContribution";
    default:
      return "<Unknown>";
  }
}

// 返回CalcSizeKeywordBehavior的字符串表示
const char* GetCalcSizeKeywordBehaviorString(
    const CalcSizeKeywordBehavior& behavior) {
  switch (behavior) {
    case CalcSizeKeywordBehavior::kAsSpecified:
      return "(保持原样)";
    case CalcSizeKeywordBehavior::kAsAuto:
      return "(视作 auto)";
    default:
      return "<Unknown>";
  }
}

std::string GetLogicalSizeString(const LogicalSize& logical_size) {
  std::ostringstream oss;
  oss << "(宽x高) {" << logical_size.inline_size << "x"
      << logical_size.block_size << "}";
  return oss.str();
}

std::string GetBoxStrutString(const BoxStrut& box_strut) {
  std::ostringstream oss;
  oss << "(上右下左) " << box_strut.block_start << "px, "
      << box_strut.inline_end << "px, " << box_strut.block_end << "px, "
      << box_strut.inline_start << "px";
  return oss.str();
}

std::string GetBoxStrutString(const PhysicalBoxStrut& box_strut) {
  std::ostringstream oss;
  oss << "(上右下左) " << box_strut.top << "px, " << box_strut.right << "px, "
      << box_strut.bottom << "px, " << box_strut.left << "px";
  return oss.str();
}

const char* GetLengthTypeString(const unsigned char type) {
  switch (static_cast<Length::Type>(type)) {
    case Length::Type::kAuto:
      return "kAuto";
    case Length::Type::kPercent:
      return "kPercent";
    case Length::Type::kFixed:
      return "kFixed";
    case Length::Type::kMinContent:
      return "kMinContent";
    case Length::Type::kMaxContent:
      return "kMaxContent";
    case Length::Type::kMinIntrinsic:
      return "kMinIntrinsic";
    case Length::Type::kFillAvailable:
      return "kFillAvailable";
    case Length::Type::kStretch:
      return "kStretch";
    case Length::Type::kFitContent:
      return "kFitContent";
    case Length::Type::kCalculated:
      return "kCalculated";
    case Length::Type::kFlex:
      return "kFlex";
    case Length::Type::kExtendToZoom:
      return "kExtendToZoom";
    case Length::Type::kDeviceWidth:
      return "kDeviceWidth";
    case Length::Type::kDeviceHeight:
      return "kDeviceHeight";
    case Length::Type::kNone:
      return "kNone";
    case Length::Type::kContent:
      return "kContent";
    default:
      return "<unknown Length::Type>";
  }
}

std::string GetNodeStr(const LayoutInputNode& layout_input_node) {
  const LayoutObject* layout_object = layout_input_node.GetLayoutBox();
  return GetNodeStr(layout_object);
}

std::string GetNodeStr(const LayoutObject* layout_object) {
  if (!layout_object) {
    return "<null LayoutObject>";
  }
  if (layout_object->IsAnonymous()) {
    return "<匿名节点>";
  }
  const Node* node = layout_object->GetNode();
  if (!node) {
    return "<null Node>";
  }
  return node->ToString().Utf8();
}

std::string GetNodeStr(const LayoutResult* layout_result) {
  if (!layout_result) {
    return "<null LayoutResult>";
  }
  if (layout_result->Status() != LayoutResult::EStatus::kSuccess) {
    return "<LayoutResult 未成功，无法获取节点信息>";
  }
  const LayoutObject* layout_object =
      layout_result->GetPhysicalFragment().GetLayoutObject();

  return GetNodeStr(layout_object);
}

std::string GetNodeStr(const PhysicalFragment& physical_fragment) {
  const LayoutObject* layout_object = physical_fragment.GetLayoutObject();
  return GetNodeStr(layout_object);
}

std::string GetFragmentGeometryString(
    const FragmentGeometry& fragment_geometry) {
  std::ostringstream oss;
  oss << "FragmentGeometry {\n"
      << "  border-box 尺寸："
      << GetLogicalSizeString(fragment_geometry.border_box_size) << ",\n"
      << "  border 大小：" << GetBoxStrutString(fragment_geometry.border)
      << ",\n"
      << "  padding：" << GetBoxStrutString(fragment_geometry.padding) << ",\n"
      << "  滚动条占用空间：" << GetBoxStrutString(fragment_geometry.scrollbar)
      << ",\n"
      << "}";
  return oss.str();
}

const char* GetLayoutCacheStatusString(const LayoutCacheStatus& status) {
  switch (status) {
    case LayoutCacheStatus::kHit:
      return "kHit (命中，可以直接使用缓存的布局结果)";
    case LayoutCacheStatus::kNeedsLayout:
      return "kNeedsLayout (未命中，需要执行完整的布局计算)";
    case LayoutCacheStatus::kNeedsSimplifiedLayout:
      return "kNeedsSimplifiedLayout "
             "(未命中，可以使用简化布局算法，避免完整重布局)";
    case LayoutCacheStatus::kCanReuseLines:
      return "kCanReuseLines (可以复用行盒，适用于文本布局优化)";
    default:
      return "<unknown LayoutCacheStatus>";
  }
}

const char* GetLineBreakerModeString(const LineBreakerMode& mode) {
  switch (mode) {
    case LineBreakerMode::kContent:
      return "kContent (布局阶段)";
    case LineBreakerMode::kMinContent:
      return "kMinContent (固有宽度阶段：计算最小)";
    case LineBreakerMode::kMaxContent:
      return "kMaxContent (固有宽度阶段：计算最大)";
    default:
      return "<unknown LineBreakerMode>";
  }
}

std::string GetMarginStrutString(const MarginStrut& margin_strut) {
  std::ostringstream oss;
  oss << "MarginStrut {\n"
      << "  正 margin: " << margin_strut.positive_margin << ",\n"
      << "  负 margin: " << margin_strut.negative_margin << ",\n"
      << "  discard_margins: " << margin_strut.discard_margins << ",\n"
      << "}\n";
  return oss.str();
}

const char* GetComputedStyleEFloatStr(const EFloat& e_float) {
  switch (e_float) {
    case EFloat::kLeft:
      return "float: left";
    case EFloat::kRight:
      return "float: right";
    case EFloat::kInlineStart:
      return "float: inline-start";
    case EFloat::kInlineEnd:
      return "float: inline-end";
    case EFloat::kNone:
      return "float: none";
    default:
      return "<unknown EFloat>";
  }
}

const char* GetComputedStyleEClearStr(const EClear& e_clear) {
  switch (e_clear) {
    case EClear::kNone:
      return "clear: none";
    case EClear::kLeft:
      return "clear: left";
    case EClear::kRight:
      return "clear: right";
    case EClear::kBoth:
      return "clear: both";
    case EClear::kInlineStart:
      return "clear: inline-start";
    case EClear::kInlineEnd:
      return "clear: inline-end";
    default:
      return "<unknown EClear>";
  }
}

const char* GetItemPositionString(const ItemPosition& item_position) {
  switch (item_position) {
    case ItemPosition::kLegacy:
      return "legacy";
    case ItemPosition::kAuto:
      return "auto";
    case ItemPosition::kNormal:
      return "normal";
    case ItemPosition::kStretch:
      return "stretch";
    case ItemPosition::kBaseline:
      return "baseline";
    case ItemPosition::kLastBaseline:
      return "last-baseline";
    case ItemPosition::kAnchorCenter:
      return "anchor-center";
    case ItemPosition::kCenter:
      return "center";
    case ItemPosition::kStart:
      return "start";
    case ItemPosition::kEnd:
      return "end";
    case ItemPosition::kSelfStart:
      return "self-start";
    case ItemPosition::kSelfEnd:
      return "self-end";
    case ItemPosition::kFlexStart:
      return "flex-start";
    case ItemPosition::kFlexEnd:
      return "flex-end";
    case ItemPosition::kLeft:
      return "left";
    case ItemPosition::kRight:
      return "right";
    default:
      return "<unknown ItemPosition>";
  }
}

const char* GetFlexLayoutAlgorithmPhaseString(int phase) {
  switch (phase) {
    case 0:  // kLayout
      return "kLayout";
    case 1:  // kRowIntrinsicSize
      return "kRowIntrinsicSize";
    case 2:  // kColumnWrapIntrinsicSize
      return "kColumnWrapIntrinsicSize";
    default:
      return "<unknown Phase>";
  }
}

const char* GetBaselineGroupString(const BaselineGroup& baseline_group) {
  switch (baseline_group) {
    case BaselineGroup::kMajor:
      return "kMajor";
    case BaselineGroup::kMinor:
      return "kMinor";
    default:
      return "<unknown BaselineGroup>";
  }
}

const char* GetFlexBreakBeforeRowString(int break_before_row) {
  switch (break_before_row) {
    case 0:  // kNotBreakBeforeRow
      return "kNotBreakBeforeRow";
    case 1:  // kAtStartOfBreakBeforeRow
      return "kAtStartOfBreakBeforeRow";
    case 2:  // kPastStartOfBreakBeforeRow
      return "kPastStartOfBreakBeforeRow";
    default:
      return "<unknown FlexBreakBeforeRow>";
  }
}

const char* GetBreakStatusString(int break_status) {
  switch (break_status) {
    case 0:  // kContinue
      return "kContinue";
    case 1:  // kNeedsEarlierBreak
      return "kNeedsEarlierBreak";
    case 2:  // kDisableFragmentation
      return "kDisableFragmentation";
    default:
      return "<unknown BreakStatus>";
  }
}

std::string GetFlexItemDataString(const FlexItemData& flex_item_data) {
  std::ostringstream oss;
  oss << "FlexItemData {\n"
      << "  node: " << ipid::GetNodeStr(flex_item_data.block_node) << ",\n"
      << "  offset: (" << flex_item_data.offset.ToString() << "),\n"
      << "  main_size: " << flex_item_data.main_axis_final_size << ",\n"
      << "  margin_block_end: " << flex_item_data.margin_block_end << ",\n"
      << "  remaining_size: " << flex_item_data.total_remaining_block_size
      << ",\n"
      << "}";
  return oss.str();
}

std::string GetFlexLineString(const FlexLine& flex_line) {
  std::ostringstream oss;
  oss << "FlexLine {\n"
      << "  cross_offset: " << flex_line.cross_axis_offset << ",\n"
      << "  line_cross_size: " << flex_line.line_cross_size << ",\n"
      << "  main_free_space: " << flex_line.main_axis_free_space << ",\n"
      << "  item_offset_adj: " << flex_line.item_offset_adjustment << ",\n"
      << "  has_seen_all: " << btos(flex_line.has_seen_all_children) << ",\n"
      << "  items_count: " << flex_line.line_items_data.size() << ",\n"
      << "}";
  return oss.str();
}

std::string GetFlexColumnBreakInfoString(
    const FlexColumnBreakInfo* break_info) {
  if (!break_info) {
    return "FlexColumnBreakInfo { nullptr }";
  }

  std::ostringstream oss;
  oss << "FlexColumnBreakInfo { block_size: "
      << break_info->column_intrinsic_block_size << " }";
  return oss.str();
}

const char* GetContentDistributionTypeString(
    const ContentDistributionType& distribution_type) {
  switch (distribution_type) {
    case ContentDistributionType::kDefault:
      return "normal";
    case ContentDistributionType::kSpaceBetween:
      return "space-between";
    case ContentDistributionType::kSpaceAround:
      return "space-around";
    case ContentDistributionType::kSpaceEvenly:
      return "space-evenly";
    case ContentDistributionType::kStretch:
      return "stretch";
    default:
      return "<Unknown>";
  }
}

const char* GetContentPositionString(const ContentPosition& content_position) {
  switch (content_position) {
    case ContentPosition::kNormal:
      return "normal";
    case ContentPosition::kBaseline:
      return "baseline";
    case ContentPosition::kLastBaseline:
      return "last-baseline";
    case ContentPosition::kCenter:
      return "center";
    case ContentPosition::kStart:
      return "start";
    case ContentPosition::kEnd:
      return "end";
    case ContentPosition::kFlexStart:
      return "flex-start";
    case ContentPosition::kFlexEnd:
      return "flex-end";
    case ContentPosition::kLeft:
      return "left";
    case ContentPosition::kRight:
      return "right";
    default:
      return "<Unknown>";
  }
}

const char* GetOverflowAlignmentString(
    const OverflowAlignment& overflow_alignment) {
  switch (overflow_alignment) {
    case OverflowAlignment::kDefault:
      return "(默认)";
    case OverflowAlignment::kUnsafe:
      return "unsafe";
    case OverflowAlignment::kSafe:
      return "safe";
    default:
      return "<Unknown>";
  }
}

std::string GetStyleContentAlignmentDataString(
    const StyleContentAlignmentData& alignment_data) {
  std::ostringstream oss;

  const char* overflow_str =
      GetOverflowAlignmentString(alignment_data.Overflow());
  const char* position_str =
      GetContentPositionString(alignment_data.GetPosition());
  const char* distribution_str =
      GetContentDistributionTypeString(alignment_data.Distribution());

  // 如果有distribution值且不是默认值，使用distribution
  if (alignment_data.Distribution() != ContentDistributionType::kDefault) {
    if (strlen(overflow_str) > 0) {
      oss << overflow_str << " " << distribution_str;
    } else {
      oss << distribution_str;
    }
  } else {
    // 否则使用position值
    if (strlen(overflow_str) > 0) {
      oss << overflow_str << " " << position_str;
    } else {
      oss << position_str;
    }
  }

  return oss.str();
}

const char* GetFlexSignString(int flex_sign) {
  switch (flex_sign) {
    case 0:  // kPositive
      return "kPositive (扩展)";
    case 1:  // kNegative
      return "kNegative (收缩)";
    default:
      return "<Unknown>";
  }
}

std::string GetFlexItemString(const FlexItem& flex_item) {
  std::ostringstream oss;
  oss << "FlexItem {\n";
  oss << "  节点: " << GetNodeStr(flex_item.block_node) << "\n";
  oss << "  索引: " << flex_item.item_index << "\n";
  oss << "  flex-grow: " << flex_item.flex_grow << "\n";
  oss << "  flex-shrink: " << flex_item.flex_shrink << "\n";
  oss << "  base_content_size: " << flex_item.base_content_size << "\n";
  oss << "  hypothetical_content_size: " << flex_item.hypothetical_content_size
      << "\n";
  oss << "  flexed_content_size: " << flex_item.flexed_content_size << "\n";
  oss << "  main_axis_min_max_sizes: "
      << GetMinMaxSizesString(flex_item.main_axis_min_max_sizes) << "\n";
  oss << "  main_axis_border_padding: " << flex_item.main_axis_border_padding
      << "\n";
  oss << "  frozen: " << btos(flex_item.frozen) << "\n";
  oss << "}";
  return oss.str();
}

}  // namespace blink::ipid
