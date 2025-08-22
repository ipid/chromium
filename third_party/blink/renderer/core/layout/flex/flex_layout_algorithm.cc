// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/layout/flex/flex_layout_algorithm.h"

#include <memory>
#include <optional>

#include "base/types/optional_util.h"
#include "third_party/blink/renderer/core/frame/web_feature.h"
#include "third_party/blink/renderer/core/layout/baseline_utils.h"
#include "third_party/blink/renderer/core/layout/block_break_token.h"
#include "third_party/blink/renderer/core/layout/constraint_space.h"
#include "third_party/blink/renderer/core/layout/constraint_space_builder.h"
#include "third_party/blink/renderer/core/layout/disable_layout_side_effects_scope.h"
#include "third_party/blink/renderer/core/layout/flex/devtools_flex_info.h"
#include "third_party/blink/renderer/core/layout/flex/flex_child_iterator.h"
#include "third_party/blink/renderer/core/layout/flex/flex_item_iterator.h"
#include "third_party/blink/renderer/core/layout/flex/flex_line.h"
#include "third_party/blink/renderer/core/layout/flex/flex_line_breaker.h"
#include "third_party/blink/renderer/core/layout/flex/layout_flexible_box.h"
#include "third_party/blink/renderer/core/layout/flex/line_flexer.h"
#include "third_party/blink/renderer/core/layout/geometry/box_strut.h"
#include "third_party/blink/renderer/core/layout/geometry/logical_size.h"
#include "third_party/blink/renderer/core/layout/ipid_debug_layout_str_utils.h"
#include "third_party/blink/renderer/core/layout/layout_box.h"
#include "third_party/blink/renderer/core/layout/layout_input_node.h"
#include "third_party/blink/renderer/core/layout/length_utils.h"
#include "third_party/blink/renderer/core/layout/logical_box_fragment.h"
#include "third_party/blink/renderer/core/layout/logical_fragment.h"
#include "third_party/blink/renderer/core/layout/physical_box_fragment.h"
#include "third_party/blink/renderer/core/layout/space_utils.h"
#include "third_party/blink/renderer/core/layout/table/table_node.h"
#include "third_party/blink/renderer/core/paint/paint_layer_scrollable_area.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/core/style/computed_style_base_constants.h"
#include "third_party/blink/renderer/core/style/computed_style_constants.h"
#include "third_party/blink/renderer/platform/geometry/layout_unit.h"
#include "third_party/blink/renderer/platform/heap/collection_support/clear_collection_scope.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"
#include "third_party/blink/renderer/platform/text/writing_mode.h"
#include "third_party/blink/renderer/platform/text/writing_mode_utils.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

// ------ ipid logging START ------
#include "third_party/blink/renderer/core/layout/ipid_debug_layout_str_utils.h"
#include "third_party/blink/renderer/platform/ipid_logging/ipid_depth_logging.h"
// ------ ipid logging END ------

namespace blink {

namespace {

template <typename Value>
class PhysicalToFlex {
  STACK_ALLOCATED();

 public:
  PhysicalToFlex(WritingDirectionMode writing_direction,
                 bool is_column,
                 Value top,
                 Value right,
                 Value bottom,
                 Value left)
      : logical_(writing_direction, top, right, bottom, left),
        is_column_(is_column) {}

  Value MainStart() const {
    return is_column_ ? logical_.BlockStart() : logical_.InlineStart();
  }
  Value MainEnd() const {
    return is_column_ ? logical_.BlockEnd() : logical_.InlineEnd();
  }
  Value CrossStart() const {
    return is_column_ ? logical_.InlineStart() : logical_.BlockStart();
  }
  Value CrossEnd() const {
    return is_column_ ? logical_.InlineEnd() : logical_.BlockEnd();
  }

 private:
  PhysicalToLogical<Value> logical_;
  bool is_column_;
};

class BaselineAccumulator {
  STACK_ALLOCATED();

 public:
  explicit BaselineAccumulator(const ComputedStyle& style)
      : font_baseline_(style.GetFontBaseline()) {}

  void AccumulateItem(const LogicalBoxFragment& fragment,
                      const LayoutUnit block_offset,
                      bool is_first_line,
                      bool is_last_line) {
    if (is_first_line) {
      if (!first_fallback_baseline_) {
        first_fallback_baseline_ =
            block_offset + fragment.FirstBaselineOrSynthesize(font_baseline_);
      }
    }

    if (is_last_line) {
      last_fallback_baseline_ =
          block_offset + fragment.LastBaselineOrSynthesize(font_baseline_);
    }
  }

  void AccumulateLine(const FlexLine& line,
                      bool is_first_line,
                      bool is_last_line) {
    if (is_first_line) {
      if (line.major_baseline != LayoutUnit::Min()) {
        first_major_baseline_ = line.cross_axis_offset + line.major_baseline;
      }
      if (line.minor_baseline != LayoutUnit::Min()) {
        first_minor_baseline_ =
            line.cross_axis_offset + line.line_cross_size - line.minor_baseline;
      }
    }

    if (is_last_line) {
      if (line.major_baseline != LayoutUnit::Min()) {
        last_major_baseline_ = line.cross_axis_offset + line.major_baseline;
      }
      if (line.minor_baseline != LayoutUnit::Min()) {
        last_minor_baseline_ =
            line.cross_axis_offset + line.line_cross_size - line.minor_baseline;
      }
    }
  }

  std::optional<LayoutUnit> FirstBaseline() const {
    if (first_major_baseline_)
      return *first_major_baseline_;
    if (first_minor_baseline_)
      return *first_minor_baseline_;
    return first_fallback_baseline_;
  }
  std::optional<LayoutUnit> LastBaseline() const {
    if (last_minor_baseline_)
      return *last_minor_baseline_;
    if (last_major_baseline_)
      return *last_major_baseline_;
    return last_fallback_baseline_;
  }

 private:
  FontBaseline font_baseline_;

  std::optional<LayoutUnit> first_major_baseline_;
  std::optional<LayoutUnit> first_minor_baseline_;
  std::optional<LayoutUnit> first_fallback_baseline_;

  std::optional<LayoutUnit> last_major_baseline_;
  std::optional<LayoutUnit> last_minor_baseline_;
  std::optional<LayoutUnit> last_fallback_baseline_;
};

LayoutUnit RowGap(const ComputedStyle& style,
                  LogicalSize percentage_resolution_size) {
  return ResolveRowGapLength(style, percentage_resolution_size.block_size)
      .value_or(LayoutUnit());
}

LayoutUnit ColumnGap(const ComputedStyle& style,
                     LogicalSize percentage_resolution_size) {
  return ResolveColumnGapLength(style, percentage_resolution_size.inline_size)
      .value_or(LayoutUnit());
}

// We build and populate the gap intersections within the flex container in an
// item by item basis. The intersections that correspond to each item are
// defined as follows:
// 1. For the first item in a line, the intersections corresponding to it will
// be:
//  - The main axis (or row) intersection (X1) of the main axis gap after the
//  item's line, with the beginning of the flex line.
// +---------------------------------------------------------------+
// | +---------+        Gap        +---------+                     |
// | |  Item   |                   |         |                     |
// | +---------+                   +---------+                     |
// |                                                               |
// X1         Row Gap                                              |
// |                                                               |
// | +---------+        Gap        +---------+                     |
// | |         |                   |         |                     |
// | +---------+                   +---------+                     |
// +---------------------------------------------------------------+
// 2. For an item in the first line (and not the first item), the
// intersections corresponding to it will be:
//  - The cross axis intersection of the cross gap before the item, with the
//  edge of the flex line (X1).
//  - The main axis intersection of the cross gap with the main gap after the
//  item's line (X2)
//  - The cross axis intersection of the cross gap with the main gap after the
//  item's line (X2).
// +-----------------------X1--------------------------------------+
// | +---------+        Gap        +---------+                     |
// | |         |                   |  Item   |           ...       |
// | +---------+                   +---------+                     |
// |                                                               |
// |         Row Gap      X2                                       |
// |                                                               |
// | +---------+        Gap        +---------+                     |
// | |         |                   |         |                     |
// | +---------+                   +---------+                     |
// +---------------------------------------------------------------+
// 3. For the last item in any line, the intersections corresponding to it
// will be:
//  - The main axis intersection of the main axis gap after the item with the
//  edge of the flex line (X1).
// +--------------------------------------------------+
// | +---------+        Gap        +---------+        |
// | |         |                   |  Item   |        |
// | +---------+                   +---------+        |
// |                                                  |
// |         Row Gap                                  X1
// |    ...                              ...          |
// +---------------------------------------------------+
// 4. For items that lie in "middle" flex lines such as
//  `Item` in the example below, the intersections corresponding to it will
//  be:
//  - The main axis intersection of the cross gap before the item with the
//  main gap before the item's line (X1).
//  - The cross axis intersection of the cross gap before the item with the
//  main gap before the item's line (X1).
//  - The cross axis intersection of the cross gap before the item with the
//  main gap after the item's line (X2).
//  - The main axis intersection of the cross gap before the item with the
//  main gap after the item's line (X2).
// +----------------------------------------------------------------------+
// |        +---------+        Gap        +---------+                     |
// |   ...  |         |                   |         |          ...        |
// |        +---------+                   +---------+                     |
// |                                                                      |
// |                Row Gap     X1                                        |
// |                                                                      |
// |        +---------+        Gap        +---------+                     |
// |   ...  |         |                   |  Item   |          ...        |
// |        +---------+                   +---------+                     |
// |            .                             .                           |
// |            .   Row Gap     X2            .                           |
// |            .                             .                           |
// |            .                             .                           |
// +----------------------------------------------------------------------+
// 2. For an item (not the first or last) in the last line, the intersections
// corresponding to it will be:
//  - The cross (or column) intersection of the cross axis gap before the
//  item, with the main axis gap before the item's line (X1).
//  - The main (or row) intersection of the cross axis gap before the item,
//  with the main axis gap before the item's line (X1).
//  - The cross axis intersection of the cross gap before the item, with the
//  edge of the flex line (X2).
// +---------------------------------------------------------------+
// | +---------+        Gap        +---------+                     |
// | |         |                   |         |                     |
// | +---------+                   +---------+                     |
// |                                                               |
// |         Row Gap     X1                                        |
// |                                                               |
// | +---------+        Gap        +---------+                     |
// | |         |                   |  Item   |                     |
// | +---------+                   +---------+                     |
// +---------------------X2----------------------------------------+
// More information on gap intersections can be found in the spec:
// https://drafts.csswg.org/css-gaps-1/#layout-painting
// TODO(javiercon): Consider refactoring this code to be able to be reused for
// masonry, by abstracting away the flex-specific logic.
class GapAccumulator {
  STACK_ALLOCATED();

 public:
  explicit GapAccumulator(LayoutUnit gap_between_items,
                          LayoutUnit gap_between_lines,
                          wtf_size_t num_lines,
                          wtf_size_t num_flex_items,
                          const BoxFragmentBuilder* container_builder,
                          bool is_column)
      : gap_between_items_(gap_between_items),
        gap_between_lines_(gap_between_lines),
        container_builder_(container_builder),
        is_column_(is_column) {
    CHECK(container_builder_);

    main_axis_gaps_.ReserveInitialCapacity(num_lines);
    cross_axis_gaps_.ReserveInitialCapacity(num_flex_items);
  }

  const GapGeometry* BuildGapGeometry() {
    const bool has_valid_main_axis_gaps =
        !main_axis_gaps_.empty() && gap_between_lines_ > LayoutUnit();
    const bool has_valid_cross_axis_gaps =
        !cross_axis_gaps_.empty() && gap_between_items_ > LayoutUnit();
    if (!has_valid_main_axis_gaps && !has_valid_cross_axis_gaps) {
      // `GapGeometry` requires at least one axis to be valid.
      return nullptr;
    }

    GapGeometry* gap_geometry =
        MakeGarbageCollected<GapGeometry>(GapGeometry::ContainerType::kFlex);

    if (is_column_) {
      // In a column flex container, the main axis gaps become the "columns" and
      // the cross axis gaps become the "rows".
      if (gap_between_lines_ > LayoutUnit()) {
        gap_geometry->SetInlineGapSize(gap_between_lines_);
        gap_geometry->SetGapIntersections(kForColumns,
                                          std::move(main_axis_gaps_));
      }
      if (gap_between_items_ > LayoutUnit()) {
        gap_geometry->SetBlockGapSize(gap_between_items_);
        gap_geometry->SetGapIntersections(kForRows,
                                          std::move(cross_axis_gaps_));
      }
    } else {
      if (gap_between_lines_ > LayoutUnit()) {
        gap_geometry->SetBlockGapSize(gap_between_lines_);
        gap_geometry->SetGapIntersections(kForRows, std::move(main_axis_gaps_));
      }
      if (gap_between_items_ > LayoutUnit()) {
        gap_geometry->SetInlineGapSize(gap_between_items_);
        gap_geometry->SetGapIntersections(kForColumns,
                                          std::move(cross_axis_gaps_));
      }
    }

    return gap_geometry;
  }

  // This adds a GapIntersection with the given main and cross offset to the
  // `destination_intersections` vector, which could be the vector for main gap
  // intersections or cross gap intersections.
  void AddGapIntersectionToResults(
      LayoutUnit main_offset,
      LayoutUnit cross_offset,
      Vector<GapIntersection>& intersection_results,
      bool is_at_edge_of_container = false) {
    LayoutUnit inline_offset = is_column_ ? cross_offset : main_offset;
    LayoutUnit block_offset = is_column_ ? main_offset : cross_offset;

    intersection_results.emplace_back(inline_offset, block_offset,
                                      is_at_edge_of_container);
  }

  // For these functions, the out parameters are:
  // - `item_cross_intersections_list` is the list of cross axis gap
  //   intersection points for the cross gap before the item.
  // - `main_intersection_offset` is the  main axis offset of cross axis gap
  //    intersection point being computed for the current item. It will be the
  //    same for all items in the line.
  // - `cross_intersection_offset` is the cross axis offset of the main axis gap
  //    intersection point being computed for the current item.
  void BuildGapIntersectionPointsForCurrentItem(
      const FlexLineVector& flex_lines,
      size_t flex_line_index,
      wtf_size_t item_index_in_line,
      LogicalOffset item_offset) {
    const FlexLine& flex_line = flex_lines[flex_line_index];

    main_intersections_after_current_line_.reserve(
        flex_line.item_indices.size());
    main_intersections_before_current_line_.reserve(
        flex_line.item_indices.size());

    // "last" here refers to last in the block direction.
    bool is_last_edge_intersection = flex_line_index == flex_lines.size() - 1;
    // "last" here refers to last in the inline direction.
    bool is_last_item_in_line =
        item_index_in_line == flex_line.item_indices.size() - 1;

    if (item_index_in_line == 0) {
      // For the first item in each line, the intersection associated with
      // them would be the intersection of the main gap after the item's line
      // with the edge of the container associated with it.
      if (!is_last_edge_intersection) {
        PopulateMainAxisGapIntersectionsForFirstItem(flex_line,
                                                     flex_lines.size());
      }
    } else {
      Vector<GapIntersection> item_cross_intersections_list;
      item_cross_intersections_list.ReserveInitialCapacity(2);

      // Gap offsets for the gap before the current item.
      LayoutUnit main_offset =
          is_column_ ? item_offset.block_offset : item_offset.inline_offset;
      LayoutUnit cross_axis_gap_start = main_offset - gap_between_items_;
      LayoutUnit cross_axis_gap_end = main_offset;
      LayoutUnit main_intersection_offset =
          (cross_axis_gap_start + cross_axis_gap_end) / 2;

      // "first" here refers to first in the block direction.
      bool is_first_edge_intersection = flex_line_index == 0;
      if (is_first_edge_intersection) {
        PopulateGapIntersectionsForFirstLine(
            flex_line, flex_lines.size(),
            main_intersection_offset, item_cross_intersections_list);
      } else if (is_last_edge_intersection) {
        PopulateGapIntersectionsForLastLine(flex_line, main_intersection_offset,
                                            item_cross_intersections_list);
      } else {
        PopulateGapIntersectionsForMiddleItem(flex_lines, flex_line_index,
                                              main_intersection_offset,
                                              item_cross_intersections_list);
      }

      cross_axis_gaps_.push_back(std::move(item_cross_intersections_list));
    }

    if (is_last_item_in_line && flex_lines.size() > 1 &&
        !is_last_edge_intersection) {
      // If we are the last item in any line except the last, we add the
      // intersection of the next main gap with the edge of the container to the
      // main axis gap intersections.
      LayoutUnit next_main_axis_gap_start = flex_line.LineCrossEnd();
      LayoutUnit next_main_axis_gap_end =
          flex_line.LineCrossEnd() + gap_between_lines_;
      LayoutUnit cross_intersection_offset =
          (next_main_axis_gap_start + next_main_axis_gap_end) / 2;
      PopulateNextMainAxisGapIntersectionsForLastItem(
          cross_intersection_offset);
    }
  }

  void PopulateGapIntersectionsForFirstLine(
      const FlexLine& flex_line,
      wtf_size_t num_lines,
      LayoutUnit main_intersection_offset,
      Vector<GapIntersection>& item_cross_intersections_list) {
    // This method assumes that the inline offset of the
    // `item_cross_intersection` is already set. If we are in the first flex
    // line, our items will be associated with two potential cross axis gap
    // intersections:
    // 1. The cross axis offset of the line.
    LayoutUnit cross_intersection_offset = flex_line.cross_axis_offset;
    AddGapIntersectionToResults(main_intersection_offset,
                                cross_intersection_offset,
                                item_cross_intersections_list,
                                /*is_at_edge_of_container=*/true);

    // 2. The main and cross intersections of the cross gap with the main
    // gap after the current line.
    LayoutUnit next_main_axis_gap_start = flex_line.LineCrossEnd();
    LayoutUnit next_main_axis_gap_end =
        flex_line.LineCrossEnd() + gap_between_lines_;
    cross_intersection_offset =
        num_lines > 1 ? (next_main_axis_gap_start + next_main_axis_gap_end) / 2
                      : flex_line.LineCrossEnd();
    AddGapIntersectionToResults(main_intersection_offset,
                                cross_intersection_offset,
                                item_cross_intersections_list);

    if (num_lines > 1) {
      AddGapIntersectionToResults(main_intersection_offset,
                                  cross_intersection_offset,
                                  main_intersections_after_current_line_);
    }
  }

  void PopulateMainAxisGapIntersectionsForFirstItem(const FlexLine& flex_line,
                                                    wtf_size_t num_lines) {
    CHECK(container_builder_);
    if (num_lines < 1) {
      return;
    }

    LayoutUnit next_main_axis_gap_start = flex_line.LineCrossEnd();
    LayoutUnit next_main_axis_gap_end =
        next_main_axis_gap_start + gap_between_lines_;

    LayoutUnit main_gap_main_intersection_offset =
        is_column_ ? container_builder_->BorderScrollbarPadding().block_start
                   : container_builder_->BorderScrollbarPadding().inline_start;
    LayoutUnit main_gap_cross_intersection_offset =
        (next_main_axis_gap_start + next_main_axis_gap_end) / 2;
    CHECK(main_intersections_after_current_line_.empty());

    GapIntersection main_gap_intersection(main_gap_main_intersection_offset,
                                          main_gap_cross_intersection_offset);
    AddGapIntersectionToResults(main_gap_main_intersection_offset,
                                main_gap_cross_intersection_offset,
                                main_intersections_after_current_line_,
                                /*is_at_edge_of_container=*/true);
  }

  void FinishedProcessingLine(wtf_size_t flex_line_idx) {
    // Because we add main axis gap intersections line by line and item by
    // item, after we add the intersections for the main axis gap after line
    // N, for the item's on line N + 1 we also have intersections for that
    // same gap which we'll want in the same ordered list. We don't have a
    // guarantee that when adding the intersections on line N+1 they will
    // strictly be after the intersections we added for the previous line, so
    // we keep a list of the intersections we added for the gap above the
    // current line, and then we merge them, sort of like MergeSort.
    // See the comment above the definition for `MergeGapIntersections` for an
    // example of why this is needed.
    if (flex_line_idx > 0) {
      Vector<GapIntersection> merged_intersections;
      MergeGapIntersections(
          !is_column_, main_intersections_before_current_line_,
          main_axis_gaps_[flex_line_idx - 1], merged_intersections);
      if (!merged_intersections.empty()) {
        main_axis_gaps_[flex_line_idx - 1] = std::move(merged_intersections);
      }
    }

    if (!main_intersections_after_current_line_.empty()) {
      main_axis_gaps_.push_back(
          std::move(main_intersections_after_current_line_));
    }

    main_intersections_after_current_line_.clear();
    main_intersections_before_current_line_.clear();
  }

  void PopulateNextMainAxisGapIntersectionsForLastItem(
      LayoutUnit cross_intersection_offset) {
    CHECK(container_builder_);
    // If we are the last item on the line, we add the intersection
    // of the next main gap with the edge of the container to the main axis
    // gap intersections.
    LayoutUnit border_scrollbar_padding =
        is_column_ ? container_builder_->BorderScrollbarPadding().block_end
                   : container_builder_->BorderScrollbarPadding().inline_end;
    LayoutUnit main_offset =
        is_column_
            ? container_builder_->InitialBorderBoxSize().block_size -
                  border_scrollbar_padding
            : container_builder_->InlineSize() - border_scrollbar_padding;
    AddGapIntersectionToResults(main_offset, cross_intersection_offset,
                                main_intersections_after_current_line_,
                                /*is_at_edge_of_container=*/true);
  }

  void PopulateGapIntersectionsForMiddleItem(
      const FlexLineVector& flex_lines,
      size_t flex_line_index,
      LayoutUnit main_intersection_offset,
      Vector<GapIntersection>& item_cross_intersections_list) {
    const FlexLine& flex_line = flex_lines[flex_line_index];
    // If we are in "middle" lines, our items will be associated with two
    // potential cross gap intersections:
    // 1. The main and cross intersections of the cross gap with the main
    // gap before the current line.
    LayoutUnit previous_main_gap_end = flex_line.cross_axis_offset;
    LayoutUnit previous_main_gap_start =
        previous_main_gap_end - gap_between_lines_;
    LayoutUnit cross_intersection_offset =
        (previous_main_gap_start + previous_main_gap_end) / 2;
    AddGapIntersectionToResults(main_intersection_offset,
                                cross_intersection_offset,
                                item_cross_intersections_list);
    AddGapIntersectionToResults(main_intersection_offset,
                                cross_intersection_offset,
                                main_intersections_before_current_line_);

    // 2. The cross and main intersections of the cross gap with the main
    // gap after the current line.
    LayoutUnit next_main_axis_gap_start = flex_line.LineCrossEnd();
    LayoutUnit next_main_axis_gap_end =
        next_main_axis_gap_start + gap_between_lines_;
    cross_intersection_offset =
        (next_main_axis_gap_start + next_main_axis_gap_end) / 2;
    AddGapIntersectionToResults(main_intersection_offset,
                                cross_intersection_offset,
                                main_intersections_after_current_line_);
    AddGapIntersectionToResults(main_intersection_offset,
                                cross_intersection_offset,
                                item_cross_intersections_list);
  }

  void PopulateGapIntersectionsForLastLine(
      const FlexLine& flex_line,
      LayoutUnit main_intersection_offset,
      Vector<GapIntersection>& item_cross_intersections_list) {
    // If we are in the last line, our items will be associated with two
    // potential cross gap intersections:
    // 1. The cross and main intersections of the cross gap with the
    // main gap before the current line.

    LayoutUnit previous_main_gap_start =
        flex_line.cross_axis_offset - gap_between_lines_;
    LayoutUnit previous_main_gap_end = flex_line.cross_axis_offset;
    LayoutUnit cross_intersection_offset =
        (previous_main_gap_start + previous_main_gap_end) / 2;
    AddGapIntersectionToResults(main_intersection_offset,
                                cross_intersection_offset,
                                main_intersections_before_current_line_);
    AddGapIntersectionToResults(main_intersection_offset,
                                cross_intersection_offset,
                                item_cross_intersections_list);

    // 2. The cross end of the line.
    cross_intersection_offset = flex_line.LineCrossEnd();
    AddGapIntersectionToResults(main_intersection_offset,
                                cross_intersection_offset,
                                item_cross_intersections_list,
                                /*is_at_edge_of_container=*/true);
  }

  // Utility function to merge two sorted lists of GapIntersections,
  // de-duplicating. Takes in two already sorted lists of GapIntersections, and
  // writes out the sorted merged list to `merged_intersections`. The final
  // GapIntersections list of a given gap is a combination of intersections of
  // items at the flex line before and flex line after that gap.
  // This is needed for a scenario such as the following:
  // +---------------------------------------------------------------+
  // | +---------+        Gap        +---------+                     |
  // | |   One   |                   |    Two  |                     |
  // | +---------+                   +---------+                     |
  // |                                                               |
  // |         Row Gap     X1   X3                                  X2
  // |                                                               |
  // | +---------------+                  +---------+                |
  // | |         Three |       Gap        |  Four   |                |
  // | +---------------+                  +---------+                |
  // +---------------------------------------------------------------+
  // If we are currently processing the main intersections for `Four`.
  // So, currently the main intersections Vector already has [X1, X2].
  // However, we also need to add X3 to the main intersections Vector,
  // but since it needs to be sorted, we can't add it to the end.
  // To solve this common issue, we merge the two already sorted lists of
  // GapIntersections, and write out the merged list to `merged_intersections`.
  // In this case this would mean merging [X1, X2] and [X3] to get [X1, X3, X2].
  void MergeGapIntersections(bool is_inline,
                             const Vector<GapIntersection>& first_list,
                             const Vector<GapIntersection>& second_list,
                             Vector<GapIntersection>& merged_intersections) {
    merged_intersections.reserve(first_list.size() + second_list.size());
    wtf_size_t first_index = 0;
    wtf_size_t second_index = 0;

    while (first_index < first_list.size() &&
           second_index < second_list.size()) {
      if (is_inline) {
        if (first_list[first_index].inline_offset ==
            second_list[second_index].inline_offset) {
          merged_intersections.push_back(first_list[first_index]);
          ++first_index;
          ++second_index;
        } else if (first_list[first_index].inline_offset <
                   second_list[second_index].inline_offset) {
          merged_intersections.push_back(first_list[first_index]);
          ++first_index;
        } else {
          merged_intersections.push_back(second_list[second_index]);
          ++second_index;
        }
      } else {
        if (first_list[first_index].block_offset ==
            second_list[second_index].block_offset) {
          merged_intersections.push_back(first_list[first_index]);
          ++first_index;
          ++second_index;
        } else if (first_list[first_index].block_offset <
                   second_list[second_index].block_offset) {
          merged_intersections.push_back(first_list[first_index]);
          ++first_index;
        } else {
          merged_intersections.push_back(second_list[second_index]);
          ++second_index;
        }
      }

      if (first_index == first_list.size() &&
          second_index < second_list.size()) {
        while (second_index < second_list.size()) {
          merged_intersections.push_back(second_list[second_index]);
          ++second_index;
        }
      } else if (second_index == second_list.size() &&
                 first_index < first_list.size()) {
        while (first_index < first_list.size()) {
          merged_intersections.push_back(first_list[first_index]);
          ++first_index;
        }
      }
    }
  }

 private:
  LayoutUnit gap_between_items_;
  LayoutUnit gap_between_lines_;
  const BoxFragmentBuilder* container_builder_ = nullptr;
  bool is_column_ = false;
  Vector<GapIntersectionList> cross_axis_gaps_;
  Vector<GapIntersectionList> main_axis_gaps_;

  // These are intermediary vectors that should be reset after processing each
  // line.
  //
  // The main axis gap intersection points for the main gap after the item.
  Vector<GapIntersection> main_intersections_after_current_line_;
  // The main axis gap intersection points for the main gap before the item.
  Vector<GapIntersection> main_intersections_before_current_line_;
};

}  // anonymous namespace

FlexLayoutAlgorithm::FlexLayoutAlgorithm(
    const LayoutAlgorithmParams& params,
    const HashMap<wtf_size_t, LayoutUnit>* cross_size_adjustments)
    : LayoutAlgorithm(params),
      is_webkit_box_(Style().IsDeprecatedFlexbox()),
      is_column_(Style().ResolvedIsColumnFlexDirection()),
      is_wrap_reverse_(Style().ResolvedIsFlexWrapReverse()),
      is_reverse_direction_(Style().ResolvedIsReverseFlexDirection()),
      is_multi_line_(!Style().ResolvedIsFlexNowrap()),
      is_horizontal_flow_(Style().IsHorizontalWritingMode() ? !is_column_
                                                            : is_column_),
      is_cross_size_definite_(IsContainerCrossSizeDefinite()),
      balance_min_line_count_(Style().ResolvedFlexBalanceMinLineCount()),
      child_percentage_size_(
          CalculateChildPercentageSize(GetConstraintSpace(),
                                       Node(),
                                       ChildAvailableSize())),
      gap_between_items_(is_column_
                             ? RowGap(Style(), child_percentage_size_)
                             : ColumnGap(Style(), child_percentage_size_)),
      gap_between_lines_(is_column_ ? ColumnGap(Style(), child_percentage_size_)
                                    : RowGap(Style(), child_percentage_size_)),
      cross_size_adjustments_(cross_size_adjustments) {
  // TODO(layout-dev): Devtools support when there are multiple fragments.
  if (Node().GetLayoutBox()->NeedsDevtoolsInfo() &&
      !InvolvedInBlockFragmentation(container_builder_))
    layout_info_for_devtools_ = std::make_unique<DevtoolsFlexInfo>();
}

void FlexLayoutAlgorithm::SetupRelayoutData(const FlexLayoutAlgorithm& previous,
                                            RelayoutType relayout_type) {
  LayoutAlgorithm::SetupRelayoutData(previous, relayout_type);

  if (relayout_type == kRelayoutIgnoringChildScrollbarChanges) {
    ignore_child_scrollbar_changes_ = true;
  } else {
    ignore_child_scrollbar_changes_ = previous.ignore_child_scrollbar_changes_;
  }
}

StyleContentAlignmentData FlexLayoutAlgorithm::ResolvedJustifyContent() const {
  if (is_webkit_box_) {
    const EBoxPack box_pack = Style().BoxPack();
    const ContentPosition position = ([&]() {
      switch (box_pack) {
        case EBoxPack::kCenter:
          return ContentPosition::kCenter;
        case EBoxPack::kJustify:
        case EBoxPack::kStart:
          return ContentPosition::kFlexStart;
        case EBoxPack::kEnd:
          return ContentPosition::kFlexEnd;
      }
    })();
    const ContentDistributionType distribution =
        box_pack == EBoxPack::kJustify ? ContentDistributionType::kSpaceBetween
                                       : ContentDistributionType::kDefault;
    return StyleContentAlignmentData(position, distribution,
                                     OverflowAlignment::kDefault);
  }

  const auto writing_direction = GetConstraintSpace().GetWritingDirection();
  const StyleContentAlignmentData& justify_content = Style().JustifyContent();

  // Coerce "left"/"right" their logical variants.
  ContentPosition position = justify_content.GetPosition();
  if (position == ContentPosition::kLeft ||
      position == ContentPosition::kRight) {
    if (is_column_) {
      if (writing_direction.IsHorizontal()) {
        // The main-axis is in the top-down direction, fallback to start.
        position = ContentPosition::kStart;
      } else {
        LogicalToPhysical physical(
            writing_direction, ContentPosition::kStart, ContentPosition::kEnd,
            ContentPosition::kStart, ContentPosition::kEnd);
        position = position == ContentPosition::kLeft ? physical.Left()
                                                      : physical.Right();
      }
    } else {
      position =
          ((position == ContentPosition::kLeft) == writing_direction.IsLtr())
              ? ContentPosition::kStart
              : ContentPosition::kEnd;
    }
  }

  return StyleContentAlignmentData(position, justify_content.Distribution(),
                                   justify_content.Overflow());
}

ItemPosition FlexLayoutAlgorithm::ResolvedAlignSelf(
    const ComputedStyle& child_style,
    bool is_out_of_flow) const {
  // Any auto-margins coerce the alignment to flex-start.
  if (!is_out_of_flow) {
    if (is_horizontal_flow_) {
      if (child_style.MarginTop().IsAuto() ||
          child_style.MarginBottom().IsAuto()) {
        return ItemPosition::kFlexStart;
      }
    } else {
      if (child_style.MarginLeft().IsAuto() ||
          child_style.MarginRight().IsAuto()) {
        return ItemPosition::kFlexStart;
      }
    }
  }

  // -webkit-box has a relatively simple alignment mapping (no need to coerce
  // "self-start", etc).
  if (is_webkit_box_) {
    switch (Style().BoxAlign()) {
      case EBoxAlignment::kBaseline:
        return ItemPosition::kBaseline;
      case EBoxAlignment::kCenter:
        return ItemPosition::kCenter;
      case EBoxAlignment::kStretch:
        return ItemPosition::kStretch;
      case EBoxAlignment::kStart:
        return ItemPosition::kFlexStart;
      case EBoxAlignment::kEnd:
        return ItemPosition::kFlexEnd;
    }
  }

  ItemPosition align =
      child_style
          .ResolvedAlignSelf(
              {ItemPosition::kStretch, OverflowAlignment::kDefault}, &Style())
          .GetPosition();
  DCHECK_NE(align, ItemPosition::kAuto);
  DCHECK_NE(align, ItemPosition::kNormal);
  DCHECK_NE(align, ItemPosition::kLeft) << "left, right are only for justify";
  DCHECK_NE(align, ItemPosition::kRight) << "left, right are only for justify";

  if (align == ItemPosition::kStart) {
    return ItemPosition::kFlexStart;
  }
  if (align == ItemPosition::kEnd) {
    return ItemPosition::kFlexEnd;
  }

  LogicalToLogical<ItemPosition> logical(
      child_style.GetWritingDirection(),
      GetConstraintSpace().GetWritingDirection(), ItemPosition::kFlexStart,
      ItemPosition::kFlexEnd, ItemPosition::kFlexStart, ItemPosition::kFlexEnd);
  if (align == ItemPosition::kSelfStart) {
    return is_column_ ? logical.InlineStart() : logical.BlockStart();
  }
  if (align == ItemPosition::kSelfEnd) {
    return is_column_ ? logical.InlineEnd() : logical.BlockEnd();
  }

  if (is_wrap_reverse_) {
    if (align == ItemPosition::kFlexStart) {
      align = ItemPosition::kFlexEnd;
    } else if (align == ItemPosition::kFlexEnd) {
      align = ItemPosition::kFlexStart;
    }
  }

  return align;
}

LayoutUnit FlexLayoutAlgorithm::MainAxisContentExtent(
    LayoutUnit sum_hypothetical_main_size) const {
  if (is_column_) {
    // Even though we only pass border_padding in the third parameter, the
    // return value includes scrollbar, so subtract scrollbar to get content
    // size.
    // We add |border_scrollbar_padding| to the fourth parameter because
    // |content_size| needs to be the size of the border box. We've overloaded
    // the term "content".
    const LayoutUnit border_scrollbar_padding =
        BorderScrollbarPadding().BlockSum();
    return ComputeBlockSizeForFragment(
               GetConstraintSpace(), Node(), BorderPadding(),
               sum_hypothetical_main_size.ClampNegativeToZero() +
                   border_scrollbar_padding,
               container_builder_.InlineSize()) -
           border_scrollbar_padding;
  }
  return ChildAvailableSize().inline_size;
}

LayoutUnit FlexLayoutAlgorithm::BaselineAscent(
    const FlexItem& item,
    const PhysicalBoxFragment& fragment) const {
  LogicalBoxFragment baseline_fragment(item.baseline_writing_direction,
                                       fragment);

  const bool is_last_baseline = item.alignment == ItemPosition::kLastBaseline;
  const auto font_baseline = Style().GetFontBaseline();
  LayoutUnit baseline =
      is_last_baseline
          ? baseline_fragment.LastBaselineOrSynthesize(font_baseline)
          : baseline_fragment.FirstBaselineOrSynthesize(font_baseline);
  if (is_wrap_reverse_ != is_last_baseline) {
    baseline = baseline_fragment.BlockSize() - baseline;
  }

  const PhysicalToFlex margins(
      GetConstraintSpace().GetWritingDirection(), is_column_,
      item.initial_margins.top, item.initial_margins.right,
      item.initial_margins.bottom, item.initial_margins.left);
  return item.baseline_group == BaselineGroup::kMajor
             ? margins.CrossStart() + baseline
             : margins.CrossEnd() + baseline;
}

LayoutUnit FlexLayoutAlgorithm::SynthesizedBaselineAscent(
    const FlexItem& item,
    const LayoutUnit block_size) const {
  const bool is_last_baseline = item.alignment == ItemPosition::kLastBaseline;
  const auto font_baseline = Style().GetFontBaseline();

  LayoutUnit baseline = LogicalBoxFragment::SynthesizedBaseline(
      font_baseline, item.baseline_writing_direction.IsFlippedLines(),
      block_size);
  if (is_wrap_reverse_ != is_last_baseline) {
    baseline = block_size - baseline;
  }

  const PhysicalToFlex margins(
      GetConstraintSpace().GetWritingDirection(), is_column_,
      item.initial_margins.top, item.initial_margins.right,
      item.initial_margins.bottom, item.initial_margins.left);
  return item.baseline_group == BaselineGroup::kMajor
             ? margins.CrossStart() + baseline
             : margins.CrossEnd() + baseline;
}

bool FlexLayoutAlgorithm::ShouldApplyAutoMinSize(const BlockNode& child) const {
  // webkit-box treats min-size: auto as 0.
  if (is_webkit_box_) {
    return false;
  }
  if (child.ShouldApplySizeContainment()) {
    return false;
  }
  // Note that the spec uses "scroll container", but it's resolved to just look
  // at the computed value of overflow not being scrollable, see:
  // https://github.com/w3c/csswg-drafts/issues/7714#issuecomment-1879319762
  const auto& child_style = child.Style();
  if (child_style.IsScrollContainer()) {
    return false;
  }
  const Length& min =
      is_horizontal_flow_ ? child_style.MinWidth() : child_style.MinHeight();
  return min.HasAuto();
}

namespace {

enum AxisEdge { kStart, kCenter, kEnd };

// Maps the resolved justify-content value to a static-position edge.
AxisEdge MainAxisStaticPositionEdge(
    const StyleContentAlignmentData& justify_content,
    bool is_reverse_direction) {
  const ContentPosition content_position = justify_content.GetPosition();
  DCHECK_NE(content_position, ContentPosition::kLeft);
  DCHECK_NE(content_position, ContentPosition::kRight);
  if (content_position == ContentPosition::kFlexEnd)
    return is_reverse_direction ? AxisEdge::kStart : AxisEdge::kEnd;

  if (content_position == ContentPosition::kCenter ||
      justify_content.Distribution() == ContentDistributionType::kSpaceAround ||
      justify_content.Distribution() == ContentDistributionType::kSpaceEvenly) {
    return AxisEdge::kCenter;
  }

  if (content_position == ContentPosition::kStart)
    return AxisEdge::kStart;
  if (content_position == ContentPosition::kEnd)
    return AxisEdge::kEnd;

  return is_reverse_direction ? AxisEdge::kEnd : AxisEdge::kStart;
}

// Maps the resolved alignment value to a static-position edge.
AxisEdge CrossAxisStaticPositionEdge(const ItemPosition alignment,
                                     bool is_wrap_reverse) {
  // AlignmentForChild already accounted for wrap-reverse for kFlexStart and
  // kFlexEnd, but not kStretch. kStretch is supposed to act like kFlexStart.
  if (is_wrap_reverse && alignment == ItemPosition::kStretch) {
    return AxisEdge::kEnd;
  }

  if (alignment == ItemPosition::kFlexEnd ||
      alignment == ItemPosition::kLastBaseline)
    return AxisEdge::kEnd;

  if (alignment == ItemPosition::kCenter)
    return AxisEdge::kCenter;

  return AxisEdge::kStart;
}

}  // namespace

void FlexLayoutAlgorithm::HandleOutOfFlowPositionedItems(
    LayoutUnit total_intrinsic_block_size,
    HeapVector<Member<LayoutBox>>& oof_children) {
  if (oof_children.empty())
    return;

  HeapVector<Member<LayoutBox>> oofs;
  std::swap(oofs, oof_children);

  bool should_process_block_end = true;
  bool should_process_block_center = true;
  const LayoutUnit previous_consumed_block_size =
      GetBreakToken() ? GetBreakToken()->ConsumedBlockSize() : LayoutUnit();

  // We will attempt to add OOFs in the fragment in which their static
  // position belongs. However, the last fragment has the most up-to-date flex
  // size information (e.g. any expanded rows, etc), so for center aligned
  // items, we could end up with an incorrect static position.
  if (InvolvedInBlockFragmentation(container_builder_)) [[unlikely]] {
    should_process_block_end = !container_builder_.DidBreakSelf() &&
                               !container_builder_.ShouldBreakInside();
    if (should_process_block_end) {
      // Recompute the total block size in case |total_intrinsic_block_size|
      // changed as a result of fragmentation.
      total_block_size_ = ComputeBlockSizeForFragment(
          GetConstraintSpace(), Node(), BorderPadding(),
          total_intrinsic_block_size, container_builder_.InlineSize());
    } else {
      LayoutUnit center = total_block_size_ / 2;
      should_process_block_center = center - previous_consumed_block_size <=
                                    FragmentainerCapacityForChildren();
    }
  }

  using InlineEdge = LogicalStaticPosition::InlineEdge;
  using BlockEdge = LogicalStaticPosition::BlockEdge;
  using LogicalAlignmentDirection =
      LogicalStaticPosition::LogicalAlignmentDirection;

  BoxStrut border_scrollbar_padding = BorderScrollbarPadding();
  border_scrollbar_padding.block_start =
      OriginalBorderScrollbarPaddingBlockStart();

  LogicalSize total_fragment_size = {container_builder_.InlineSize(),
                                     total_block_size_};
  total_fragment_size =
      ShrinkLogicalSize(total_fragment_size, border_scrollbar_padding);

  const StyleContentAlignmentData justify_content = ResolvedJustifyContent();
  const AxisEdge main_axis_edge =
      MainAxisStaticPositionEdge(justify_content, is_reverse_direction_);

  for (LayoutBox* oof_child : oofs) {
    BlockNode child(oof_child);

    const ItemPosition position =
        ResolvedAlignSelf(child.Style(), /* is_out_of_flow */ true);
    AxisEdge cross_axis_edge =
        CrossAxisStaticPositionEdge(position, is_wrap_reverse_);

    AxisEdge inline_axis_edge = is_column_ ? cross_axis_edge : main_axis_edge;
    AxisEdge block_axis_edge = is_column_ ? main_axis_edge : cross_axis_edge;

    InlineEdge inline_edge;
    BlockEdge block_edge;
    LogicalOffset offset = border_scrollbar_padding.StartOffset();

    // Determine the static-position based off the axis-edge.
    if (block_axis_edge == AxisEdge::kStart) {
      DCHECK(!IsBreakInside(GetBreakToken()));
      block_edge = BlockEdge::kBlockStart;
    } else if (block_axis_edge == AxisEdge::kCenter) {
      if (!should_process_block_center) {
        oof_children.emplace_back(oof_child);
        continue;
      }
      block_edge = BlockEdge::kBlockCenter;
      offset.block_offset += total_fragment_size.block_size / 2;
    } else {
      if (!should_process_block_end) {
        oof_children.emplace_back(oof_child);
        continue;
      }
      block_edge = BlockEdge::kBlockEnd;
      offset.block_offset += total_fragment_size.block_size;
    }

    if (inline_axis_edge == AxisEdge::kStart) {
      inline_edge = InlineEdge::kInlineStart;
    } else if (inline_axis_edge == AxisEdge::kCenter) {
      inline_edge = InlineEdge::kInlineCenter;
      offset.inline_offset += total_fragment_size.inline_size / 2;
    } else {
      inline_edge = InlineEdge::kInlineEnd;
      offset.inline_offset += total_fragment_size.inline_size;
    }

    // Make the child offset relative to our fragment.
    offset.block_offset -= previous_consumed_block_size;

    LogicalAlignmentDirection align_self_direction =
        is_column_ ? LogicalAlignmentDirection::kInline
                   : LogicalAlignmentDirection::kBlock;

    container_builder_.AddOutOfFlowChildCandidate(
        child, offset, inline_edge, block_edge, align_self_direction);
  }
}

void FlexLayoutAlgorithm::SetReadingFlowNodes(
    const FlexLineVector& flex_lines) {
  const auto& style = Style();
  const EReadingFlow reading_flow = style.ReadingFlow();
  if (reading_flow != EReadingFlow::kFlexVisual &&
      reading_flow != EReadingFlow::kFlexFlow) {
    return;
  }
  HeapVector<Member<blink::Node>> reading_flow_nodes;
  reading_flow_nodes.ReserveInitialCapacity(flex_items_.size());
  // Add flex item if it is a DOM node
  auto add_item_if_needed = [&](const wtf_size_t item_index) {
    if (blink::Node* node = flex_items_[item_index].block_node.GetDOMNode()) {
      reading_flow_nodes.push_back(node);
    }
  };
  // Given CSS reading-flow, flex-flow, flex-direction; read values
  // in correct order.
  auto add_flex_items = [&](const FlexLine& line) {
    if (reading_flow == EReadingFlow::kFlexFlow && is_reverse_direction_) {
      for (const wtf_size_t item_index : base::Reversed(line.item_indices)) {
        add_item_if_needed(item_index);
      }
    } else {
      for (const wtf_size_t item_index : line.item_indices) {
        add_item_if_needed(item_index);
      }
    }
  };
  if (reading_flow == EReadingFlow::kFlexFlow && is_wrap_reverse_) {
    for (const auto& line : base::Reversed(flex_lines)) {
      add_flex_items(line);
    }
  } else {
    for (const auto& line : flex_lines) {
      add_flex_items(line);
    }
  }
  container_builder_.SetReadingFlowNodes(std::move(reading_flow_nodes));
}

bool FlexLayoutAlgorithm::IsContainerCrossSizeDefinite() const {
  // A column flexbox's cross axis is an inline size, so is definite.
  if (is_column_)
    return true;

  return ChildAvailableSize().block_size != kIndefiniteSize;
}

bool FlexLayoutAlgorithm::DoesItemStretch(const BlockNode& child,
                                          ItemPosition alignment) const {
  // Note: Unresolvable % cross size doesn't count as auto for stretchability.
  // As discussed in https://github.com/w3c/csswg-drafts/issues/4312.
  return alignment == ItemPosition::kStretch &&
         DoesItemComputedCrossSizeHaveAuto(child);
}

bool FlexLayoutAlgorithm::DoesItemComputedCrossSizeHaveAuto(
    const BlockNode& child) const {
  const ComputedStyle& child_style = child.Style();
  if (is_horizontal_flow_) {
    return child_style.Height().HasAuto();
  }
  return child_style.Width().HasAuto();
}

bool FlexLayoutAlgorithm::WillChildCrossSizeBeContainerCrossSize(
    const BlockNode& child,
    ItemPosition alignment) const {
  return !is_multi_line_ && is_cross_size_definite_ &&
         DoesItemStretch(child, alignment);
}

ConstraintSpace FlexLayoutAlgorithm::BuildSpaceForIntrinsicInlineSize(
    const BlockNode& child,
    ItemPosition alignment) const {
  MinMaxConstraintSpaceBuilder builder(GetConstraintSpace(), Style(), child,
                                       /* is_new_fc */ true);
  builder.SetAvailableBlockSize(ChildAvailableSize().block_size);
  builder.SetPercentageResolutionBlockSize(child_percentage_size_.block_size);
  if (!is_column_ && !is_multi_line_ && alignment == ItemPosition::kStretch) {
    builder.SetBlockAutoBehavior(AutoSizeBehavior::kStretchExplicit);
  }
  return builder.ToConstraintSpace();
}

ConstraintSpace FlexLayoutAlgorithm::BuildSpaceForFlexBasis(
    const BlockNode& flex_item) const {
  ConstraintSpaceBuilder space_builder(GetConstraintSpace(),
                                       flex_item.Style().GetWritingDirection(),
                                       /* is_new_fc */ true);
  SetOrthogonalFallbackInlineSizeIfNeeded(Style(), flex_item, &space_builder);

  // This space is only used for resolving lengths, not for layout. We only
  // need the available and percentage sizes.
  space_builder.SetAvailableSize(ChildAvailableSize());
  space_builder.SetPercentageResolutionSize(child_percentage_size_);
  return space_builder.ToConstraintSpace();
}

const ConstraintSpace FlexLayoutAlgorithm::BuildSpaceForLayout(
    const BlockNode& node,
    ItemPosition alignment,
    bool is_initial_block_size_indefinite,
    std::optional<LayoutUnit> override_inline_size,
    std::optional<LayoutUnit> main_axis_final_size,
    std::optional<LayoutUnit> line_cross_size,
    std::optional<LayoutUnit> block_offset_for_fragmentation,
    bool min_block_size_should_encompass_intrinsic_size) const {
  IpidDepthLog ipid_depth_log(
      "flex_layout_algorithm.cc: FlexLayoutAlgorithm::BuildSpaceForLayout");

  ipid_depth_log.FPrint(
      "为 flex 子元素 {} 构建布局空间约束。\n"
      "输入参数：\n"
      "- 对齐方式 alignment: {}\n"
      "- 初始高度是否不确定 is_initial_block_size_indefinite: {}\n"
      "- 覆盖宽度 override_inline_size: {}\n"
      "- 主轴最终尺寸 main_axis_final_size: {}\n"
      "- 行交叉轴尺寸 line_cross_size: {}\n"
      "- 分页偏移量 block_offset_for_fragmentation: {}\n"
      "- 最小高度应包含内在尺寸 "
      "min_block_size_should_encompass_intrinsic_size: {}\n"
      "容器属性：\n"
      "- 容器是否为列方向 is_column_: {}\n"
      "- 是否为多行容器 is_multi_line_: {}\n"
      "- 交叉轴尺寸是否确定 is_cross_size_definite_: {}",
      ipid::GetNodeStr(node), ipid::GetItemPositionString(alignment),
      ipid::btos(is_initial_block_size_indefinite),
      override_inline_size ? std::to_string(override_inline_size->ToFloat())
                           : "无",
      main_axis_final_size ? std::to_string(main_axis_final_size->ToFloat())
                           : "无",
      line_cross_size ? std::to_string(line_cross_size->ToFloat()) : "无",
      block_offset_for_fragmentation
          ? std::to_string(block_offset_for_fragmentation->ToFloat())
          : "无",
      ipid::btos(min_block_size_should_encompass_intrinsic_size),
      ipid::btos(is_column_), ipid::btos(is_multi_line_),
      ipid::btos(is_cross_size_definite_));

  ipid_depth_log.FPrint(
      "第 1 步：创建 ConstraintSpaceBuilder。\n"
      "基于父容器的约束空间和子元素的书写方向创建构建器，标记为新的格式化上下文"
      " (is_new_fc=true)，"
      "这意味着该子元素将创建独立的布局环境。");

  ConstraintSpaceBuilder builder(GetConstraintSpace(),
                                 node.Style().GetWritingDirection(),
                                 /* is_new_fc */ true);

  ipid_depth_log.FPrint(
      "第 2 步：设置正交回退宽度。\n"
      "当子元素的书写方向与父容器不平行时（如父容器为水平书写，子元素为垂直书写"
      "），"
      "需要设置一个回退宽度值，以确保布局的稳定性。");

  SetOrthogonalFallbackInlineSizeIfNeeded(Style(), node, &builder);
  builder.SetIsPaintedAtomically(true);

  ipid_depth_log.FPrint(
      "第 3 步：设置缓存策略。\n"
      "当前 line_cross_size 为 "
      "{}。如果没有行交叉轴尺寸，说明这是一次测量阶段的布局，"
      "设置缓存槽为 kMeasure，这样可以缓存测量阶段的布局结果。",
      line_cross_size ? "有值" : "无");

  // Until we have a line cross-size, everything is a measure pass.
  if (!line_cross_size) {
    builder.SetCacheSlot(LayoutResultCacheSlot::kMeasure);
    ipid_depth_log.FPrint("由于没有行交叉轴尺寸，将缓存槽设为 kMeasure。");
  }

  LogicalSize available_size = ChildAvailableSize();
  LogicalSize percentage_size = child_percentage_size_;

  ipid_depth_log.FPrint(
      "第 4 步：获取基础尺寸。\n"
      "从父容器获取子元素的可用空间：{}\n"
      "从容器中获取用于百分比解析的尺寸：{}\n"
      "这些尺寸将作为构建子元素布局空间的基础。",
      ipid::GetLogicalSizeString(available_size),
      ipid::GetLogicalSizeString(percentage_size));

  // If we are balancing with a minimum line-count, divide the cross-axis
  // available-space if definite.
  if (balance_min_line_count_) {
    const LayoutUnit gap_size =
        (*balance_min_line_count_ - 1) * gap_between_lines_;

    ipid_depth_log.FPrint(
        "第 5 步：处理最小行数平衡。\n"
        "当前启用了最小行数平衡，balance_min_line_count_ = {}，行间间距 "
        "gap_between_lines_ = {}。\n"
        "计算总间距：({} - 1) × {} = "
        "{}，然后将交叉轴的可用空间除以行数进行平衡分配。",
        *balance_min_line_count_, gap_between_lines_, *balance_min_line_count_,
        gap_between_lines_, gap_size);

    if (is_column_) {
      if (available_size.inline_size != kIndefiniteSize) {
        LayoutUnit original_inline_size = available_size.inline_size;
        available_size.inline_size =
            (available_size.inline_size - gap_size) / *balance_min_line_count_;
        ipid_depth_log.FPrint(
            "列方向容器：将宽度从 {} 调整为 ({} - {}) / {} = {}。",
            original_inline_size, original_inline_size, gap_size,
            *balance_min_line_count_, available_size.inline_size);
      }
    } else {
      if (available_size.block_size != kIndefiniteSize) {
        LayoutUnit original_block_size = available_size.block_size;
        available_size.block_size =
            (available_size.block_size - gap_size) / *balance_min_line_count_;
        ipid_depth_log.FPrint(
            "行方向容器：将高度从 {} 调整为 ({} - {}) / {} = {}。",
            original_block_size, original_block_size, gap_size,
            *balance_min_line_count_, available_size.block_size);
      }
    }
  } else {
    ipid_depth_log.FPrint("第 5 步：跳过最小行数平衡（未启用）。");
  }

  ipid_depth_log.FPrint(
      "第 6 步：根据 flex 方向设置空间尺寸。\n"
      "当前容器为{}方向。根据 flex "
      "方向的不同，主轴和交叉轴对应的尺寸维度也不同。",
      is_column_ ? "列" : "行");

  if (is_column_) {
    ipid_depth_log.FPrint(
        "列方向布局：主轴为高度方向，交叉轴为宽度方向。\n"
        "- override_inline_size: {}\n"
        "- line_cross_size: {}\n"
        "- main_axis_final_size: {}",
        override_inline_size ? std::to_string(override_inline_size->ToFloat())
                             : "无",
        line_cross_size ? std::to_string(line_cross_size->ToFloat()) : "无",
        main_axis_final_size ? std::to_string(main_axis_final_size->ToFloat())
                             : "无");

    if (override_inline_size) {
      DCHECK(!line_cross_size)
          << "We only override inline size when we are calculating intrinsic "
             "width of multiline column flexboxes, and we don't do any "
             "stretching during the intrinsic width calculation.";
      available_size.inline_size = *override_inline_size;
      builder.SetIsFixedInlineSize(true);
      ipid_depth_log.FPrint(
          "使用覆盖宽度：将可用宽度设为 {}，并标记宽度为固定值。"
          "这通常发生在计算多行列 flex 容器的固有宽度时。",
          *override_inline_size);
    } else if (line_cross_size) {
      available_size.inline_size = *line_cross_size;
      ipid_depth_log.FPrint(
          "使用行交叉轴尺寸：将可用宽度设为 {}。"
          "这是在实际布局阶段，已确定该行在交叉轴上的尺寸。",
          *line_cross_size);
    }
    if (main_axis_final_size) {
      available_size.block_size = *main_axis_final_size;
      builder.SetIsFixedBlockSize(true);
      ipid_depth_log.FPrint(
          "使用主轴最终尺寸：将可用高度设为 {}，并标记高度为固定值。"
          "这意味着子元素在主轴方向的尺寸已经确定。",
          *main_axis_final_size);
    }
  } else {
    DCHECK(!override_inline_size);
    ipid_depth_log.FPrint(
        "行方向布局：主轴为宽度方向，交叉轴为高度方向。\n"
        "- line_cross_size: {}\n"
        "- main_axis_final_size: {}",
        line_cross_size ? std::to_string(line_cross_size->ToFloat()) : "无",
        main_axis_final_size ? std::to_string(main_axis_final_size->ToFloat())
                             : "无");

    if (line_cross_size) {
      available_size.block_size = *line_cross_size;
      ipid_depth_log.FPrint(
          "使用行交叉轴尺寸：将可用高度设为 {}。"
          "这是在实际布局阶段，已确定该行在交叉轴上的尺寸。",
          *line_cross_size);
    }
    if (main_axis_final_size) {
      available_size.inline_size = *main_axis_final_size;
      builder.SetIsFixedInlineSize(true);
      ipid_depth_log.FPrint(
          "使用主轴最终尺寸：将可用宽度设为 {}，并标记宽度为固定值。"
          "这意味着子元素在主轴方向的尺寸已经确定。",
          *main_axis_final_size);
    }
  }

  // We guard against an indefinite cross-axis size as if we are an orthogonal
  // item, the fallback-size may be definite.
  const bool is_cross_size_definite =
      (!is_multi_line_ && is_cross_size_definite_) || line_cross_size;

  ipid_depth_log.FPrint(
      "第 7 步：处理交叉轴拉伸行为。\n"
      "判断交叉轴尺寸是否确定：is_cross_size_definite = {}\n"
      "计算逻辑：(!is_multi_line_ && is_cross_size_definite_) || "
      "line_cross_size\n"
      "         = (!{} && {}) || {}\n"
      "         = {} || {}\n"
      "         = {}\n"
      "当前对齐方式：{}",
      ipid::btos(is_cross_size_definite), ipid::btos(is_multi_line_),
      ipid::btos(is_cross_size_definite_), line_cross_size ? "有值" : "无值",
      ipid::btos(!is_multi_line_ && is_cross_size_definite_),
      ipid::btos(!!line_cross_size), ipid::btos(is_cross_size_definite),
      ipid::GetItemPositionString(alignment));

  if (is_cross_size_definite && alignment == ItemPosition::kStretch) {
    if (is_column_) {
      builder.SetInlineAutoBehavior(AutoSizeBehavior::kStretchExplicit);
      ipid_depth_log.FPrint(
          "列方向容器 + stretch 对齐：设置宽度的 auto 行为为显式拉伸。"
          "这意味着当子元素的宽度为 auto 时，会拉伸填满可用的交叉轴空间。");
    } else {
      builder.SetBlockAutoBehavior(AutoSizeBehavior::kStretchExplicit);
      ipid_depth_log.FPrint(
          "行方向容器 + stretch 对齐：设置高度的 auto 行为为显式拉伸。"
          "这意味着当子元素的高度为 auto 时，会拉伸填满可用的交叉轴空间。");
    }
  } else {
    ipid_depth_log.FPrint(
        "不设置拉伸行为，原因：交叉轴尺寸不确定 ({}) 或对齐方式不是 stretch "
        "({})。",
        ipid::btos(!is_cross_size_definite),
        ipid::btos(alignment != ItemPosition::kStretch));
  }

  if (is_initial_block_size_indefinite) {
    DCHECK(is_column_);
    builder.SetIsInitialBlockSizeIndefinite(true);

    ipid_depth_log.FPrint(
        "第 8 步：处理初始高度不确定的情况。\n"
        "当前子元素的初始高度被认为是不确定的，这只在列方向容器中出现。"
        "设置 IsInitialBlockSizeIndefinite(true) 来告知布局算法这一点。");

    // When measuring for column layout set our extrinsic constraints to
    // indefinite.
    // This isn't explicitly required (e.g. all tests will pass without this),
    // however it makes the measure cache more efficient.
    if (!main_axis_final_size) {
      available_size.block_size = kIndefiniteSize;
      percentage_size.block_size = kIndefiniteSize;
      ipid_depth_log.FPrint(
          "由于没有主轴最终尺寸，将可用高度和百分比解析高度都设为不确定值 "
          "(-1)。"
          "这样做的目的是提高测量阶段缓存的效率，避免不必要的重复计算。");
    }
  } else {
    ipid_depth_log.FPrint("第 8 步：初始高度确定，跳过相关处理。");
  }

  if (block_offset_for_fragmentation &&
      GetConstraintSpace().HasBlockFragmentation()) {
    ipid_depth_log.FPrint(
        "第 9 步：处理分页布局。\n"
        "当前处于分页环境中，需要设置分页相关的参数。\n"
        "分页偏移量：{}\n"
        "最小高度应包含内在尺寸：{}",
        *block_offset_for_fragmentation,
        ipid::btos(min_block_size_should_encompass_intrinsic_size));

    if (min_block_size_should_encompass_intrinsic_size) {
      builder.SetMinBlockSizeShouldEncompassIntrinsicSize();
      ipid_depth_log.FPrint(
          "设置最小高度应包含内在尺寸，这确保元素在分页时不会被过度压缩。");
    }
    SetupSpaceBuilderForFragmentation(
        container_builder_, node, *block_offset_for_fragmentation, &builder);
    ipid_depth_log.FPrint(
        "调用 SetupSpaceBuilderForFragmentation 设置分页相关的空间构建参数。");
  } else {
    ipid_depth_log.FPrint(
        "第 9 步：无需处理分页（非分页环境或无分页偏移量）。");
  }

  ipid_depth_log.FPrint(
      "第 10 步：设置最终的空间参数并构建 ConstraintSpace。\n"
      "最终的可用尺寸：{}\n"
      "最终的百分比解析尺寸：{}",
      ipid::GetLogicalSizeString(available_size),
      ipid::GetLogicalSizeString(percentage_size));

  builder.SetAvailableSize(available_size);
  builder.SetPercentageResolutionSize(percentage_size);
  ConstraintSpace result_space = builder.ToConstraintSpace();

  ipid_depth_log.FPrint("成功构建了子元素 {} 的布局空间约束：\n{}",
                        ipid::GetNodeStr(node),
                        ipid::GetConstraintSpaceString(result_space));

  return result_space;
}

void FlexLayoutAlgorithm::ConstructAndAppendFlexItems(
    Phase phase,
    HeapVector<Member<LayoutBox>>* oof_children) {
  IpidDepthLog ipid_depth_log(
      "FlexLayoutAlgorithm::ConstructAndAppendFlexItems");

  ipid_depth_log.FPrint(
      "开始构建和添加 flex 项目，当前阶段为 {}。这个函数的作用是遍历 flex "
      "容器的子元素，计算每个 flex 项目的各种属性（如 flex-basis、min-max "
      "宽度、对齐方式等），然后将它们添加到 flex_items_ 列表中供后续布局使用。",
      ipid::GetFlexLayoutAlgorithmPhaseString(static_cast<int>(phase)));

  wtf_size_t item_index = 0;
  FlexChildIterator iterator(Node());
  flex_items_.ReserveInitialCapacity(iterator.size());

  ipid_depth_log.FPrint(
      "使用 FlexChildIterator 遍历 flex 容器的子元素，迭代器发现了 {} "
      "个子元素。FlexChildIterator 会按照 CSS order "
      "属性的顺序来提供子元素。预先为 flex_items_ 列表预留 {} 个元素的容量。",
      iterator.size(), iterator.size());

  for (BlockNode child = iterator.NextChild(); child;
       child = iterator.NextChild()) {
    ipid_depth_log.FPrint(
        "正在处理子元素 {}（第 {} 个），检查其是否为 out-of-flow 定位元素。",
        ipid::GetNodeStr(child), item_index);

    if (child.IsOutOfFlowPositioned()) {
      ipid_depth_log.FPrint(
          "子元素 {} 是 out-of-flow 定位元素（position: absolute 或 "
          "fixed），在 flex 布局中不参与 flex 项目的计算，仅在 Layout "
          "阶段将其添加到 oof_children 列表中。",
          ipid::GetNodeStr(child));
      if (phase == Phase::kLayout) {
        DCHECK(oof_children);
        oof_children->emplace_back(child.GetLayoutBox());
        ipid_depth_log.FPrint(
            "当前阶段为 kLayout，将 out-of-flow 子元素 {} 添加到 oof_children "
            "列表中。",
            ipid::GetNodeStr(child));
      }
      continue;
    }

    const ComputedStyle& child_style = child.Style();
    const ItemPosition alignment = ResolvedAlignSelf(child_style);

    ipid_depth_log.FPrint(
        "子元素 {} 是正常的 flex 项目，获取其计算样式。通过 ResolvedAlignSelf "
        "解析得到的对齐方式为 {}，这决定了该 flex 项目在交叉轴上的对齐行为。",
        ipid::GetNodeStr(child), ipid::GetItemPositionString(alignment));

    std::optional<LayoutUnit> max_content_contribution;
    if (phase == Phase::kColumnWrapIntrinsicSize) {
      ipid_depth_log.FPrint(
          "当前阶段为 kColumnWrapIntrinsicSize，需要计算子元素 {} "
          "对列方向换行固有宽度的贡献值。这个阶段专门用于计算多行 flex "
          "容器的固有宽度。",
          ipid::GetNodeStr(child));

      auto space = BuildSpaceForIntrinsicInlineSize(child, alignment);
      ipid_depth_log.FPrint(
          "为子元素 {} "
          "构建用于计算固有宽度的约束空间：{}"
          "。这个空间用于让子元素计算其固有最小和最大宽度。",
          ipid::GetNodeStr(child), ipid::GetConstraintSpaceString(space));

      MinMaxSizesResult child_contributions =
          ComputeMinAndMaxContentContribution(Style(), child, space);
      max_content_contribution = child_contributions.sizes.max_size;

      ipid_depth_log.FPrint(
          "调用 ComputeMinAndMaxContentContribution 计算子元素 {} "
          "对父容器固有宽度的贡献值：{}。取其中的最大内容贡献值 {} 作为 "
          "max_content_contribution。",
          ipid::GetNodeStr(child),
          ipid::GetMinMaxSizesResultString(child_contributions),
          max_content_contribution.value());

      BoxStrut child_margins =
          ComputeMarginsFor(space, child.Style(), GetConstraintSpace());
      child_contributions.sizes += child_margins.InlineSum();

      ipid_depth_log.FPrint(
          "计算子元素 {} 的 margin：{}，将其宽度方向的 margin 总和 {} "
          "加到贡献值中。调整后的贡献值：{}。",
          ipid::GetNodeStr(child), ipid::GetBoxStrutString(child_margins),
          child_margins.InlineSum(),
          ipid::GetMinMaxSizesString(child_contributions.sizes));

      largest_min_content_contribution_ =
          std::max(child_contributions.sizes.min_size,
                   largest_min_content_contribution_);

      ipid_depth_log.FPrint(
          "更新全局的最大固有最小宽度贡献值：当前最大值为 {}，子元素 {} "
          "的贡献值为 {}，取两者较大值作为新的最大值 {}。",
          largest_min_content_contribution_, ipid::GetNodeStr(child),
          child_contributions.sizes.min_size,
          std::max(child_contributions.sizes.min_size,
                   largest_min_content_contribution_));
    }

    const auto child_writing_mode = child_style.GetWritingMode();
    const bool is_main_axis_inline_axis =
        IsHorizontalWritingMode(child_writing_mode) == is_horizontal_flow_;

    ipid_depth_log.FPrint(
        "分析子元素 {} 的书写模式和主轴方向：\n"
        "- 子元素的书写模式为水平模式: {}，\n"
        "- flex 容器为水平流: {}，\n"
        "- 因此 flex 主轴是否与子元素的宽度方向一致: {}，\n"
        "\n这个判断对后续的 flex-basis "
        "计算很重要，因为它决定了是沿宽度还是高度方向进行 flexbox 布局。",
        ipid::GetNodeStr(child),
        ipid::btos(IsHorizontalWritingMode(child_writing_mode)),
        ipid::btos(is_horizontal_flow_), ipid::btos(is_main_axis_inline_axis));

    ConstraintSpace flex_basis_space = BuildSpaceForFlexBasis(child);

    ipid_depth_log.FPrint(
        "为子元素 {} 构建用于计算 flex-basis "
        "的约束空间：\n{}\n这个空间专门用于解析 flex-basis "
        "属性值，提供合适的可用宽度和高度上下文。",
        ipid::GetNodeStr(child),
        ipid::GetConstraintSpaceString(flex_basis_space));

    PhysicalBoxStrut physical_child_margins =
        ComputePhysicalMargins(flex_basis_space, child_style);

    BoxStrut border_padding_in_child_writing_mode =
        ComputeBorders(flex_basis_space, child) +
        ComputePadding(flex_basis_space, child_style);

    PhysicalBoxStrut physical_border_padding(
        border_padding_in_child_writing_mode.ConvertToPhysical(
            child_style.GetWritingDirection()));

    ipid_depth_log.FPrint(
        "计算子元素 {} 的盒子模型数据：物理 margin 为 {}，子元素书写模式下的 "
        "border+padding 为 {}，转换为物理 border+padding 为 "
        "{}。这些值用于后续的 flex-basis 和最终尺寸计算。",
        ipid::GetNodeStr(child),
        ipid::GetBoxStrutString(physical_child_margins),
        ipid::GetBoxStrutString(border_padding_in_child_writing_mode),
        ipid::GetBoxStrutString(physical_border_padding));

    const uint8_t main_axis_auto_margin_count =
        is_horizontal_flow_ ? child_style.MarginLeft().IsAuto() +
                                  child_style.MarginRight().IsAuto()
                            : child_style.MarginTop().IsAuto() +
                                  child_style.MarginBottom().IsAuto();
    const LayoutUnit main_axis_border_padding =
        is_horizontal_flow_ ? physical_border_padding.HorizontalSum()
                            : physical_border_padding.VerticalSum();

    ipid_depth_log.FPrint(
        "统计子元素 {} 主轴方向的 auto margin 数量：{}。计算主轴方向的 "
        "border+padding 总和：{}。auto margin 在 flex "
        "布局中有特殊作用，可以自动分配剩余空间；主轴方向的 border+padding "
        "用于计算内容区域的实际可用空间。",
        ipid::GetNodeStr(child), main_axis_auto_margin_count,
        main_axis_border_padding);
    const auto child_space =
        BuildSpaceForLayout(child, alignment,
                            /* is_initial_block_size_indefinite */ is_column_ &&
                                !is_main_axis_inline_axis,
                            max_content_contribution);

    ipid_depth_log.FPrint(
        "为子元素 {} "
        "构建用于布局的约束空间：{}。is_initial_block_size_indefinite 参数为 "
        "{}，这表示当 flex "
        "容器是列方向且子元素主轴不是宽度方向时，初始高度是不确定的。max_"
        "content_contribution 参数为 {}，来自前面 kColumnWrapIntrinsicSize "
        "阶段的计算结果。",
        ipid::GetNodeStr(child), ipid::GetConstraintSpaceString(child_space),
        ipid::btos(is_column_ && !is_main_axis_inline_axis),
        max_content_contribution.has_value()
            ? std::to_string(max_content_contribution.value().ToDouble()) + "px"
            : "nullopt");

    bool depends_on_min_max_sizes = false;
    auto MinMaxSizesFunc = [&](SizeType type) -> MinMaxSizesResult {
      depends_on_min_max_sizes = true;
      // We want the child's intrinsic inline sizes in its writing mode, so
      // pass child's writing mode as the first parameter, which is nominally
      // |container_writing_mode|.
      return child.ComputeMinMaxSizes(child_writing_mode, type, child_space);
    };

    ipid_depth_log.FPrint(
        "为子元素 {} 定义 MinMaxSizesFunc "
        "闭包函数。这个函数用于按需计算子元素的固有最小最大宽度，采用子元素的书"
        "写模式，避免不必要的计算。depends_on_min_max_sizes 标志初始化为 "
        "false，当实际调用固有宽度计算时会被设为 true。",
        ipid::GetNodeStr(child));

    auto InlineSizeFunc = [&]() -> LayoutUnit {
      return CalculateInitialFragmentGeometry(child_space, child,
                                              /* break_token */ nullptr)
          .border_box_size.inline_size;
    };

    ipid_depth_log.FPrint(
        "为子元素 {} 定义 InlineSizeFunc "
        "闭包函数。这个函数用于按需计算子元素的宽度，通过 "
        "CalculateInitialFragmentGeometry 来获取边框盒的宽度。",
        ipid::GetNodeStr(child));

    const LayoutResult* layout_result = nullptr;
    auto BlockSizeFunc = [&](SizeType type) -> LayoutUnit {
      // This function mirrors the logic within `BlockNode::ComputeMinMaxSizes`.

      // Don't apply any special aspect-ratio treatment for replaced elements.
      if (child.IsReplaced()) {
        return ComputeReplacedSize(child, child_space,
                                   border_padding_in_child_writing_mode,
                                   ReplacedSizeMode::kIgnoreBlockLengths)
            .block_size;
      }

      const bool has_aspect_ratio = !child_style.AspectRatio().IsAuto();
      if (has_aspect_ratio && type == SizeType::kContent) {
        const LayoutUnit inline_size = InlineSizeFunc();
        if (inline_size != kIndefiniteSize) {
          return BlockSizeFromAspectRatio(border_padding_in_child_writing_mode,
                                          child_style.LogicalAspectRatio(),
                                          child_style.BoxSizingForAspectRatio(),
                                          inline_size);
        }
      }

      // We may be able to avoid layout if we have size-containment, or a
      // default size.
      LayoutUnit intrinsic_size = CalculateIntrinsicBlockSizeIgnoringChildren(
          child, border_padding_in_child_writing_mode +
                     ComputeScrollbarsForNonAnonymous(child));

      if (intrinsic_size == kIndefiniteSize) {
        if (!layout_result) {
          std::optional<DisableLayoutSideEffectsScope> disable_side_effects;
          if (phase != Phase::kLayout && !child.GetLayoutBox()->NeedsLayout()) {
            disable_side_effects.emplace();
          }
          layout_result = child.Layout(child_space);
          DCHECK(layout_result);
        }
        intrinsic_size = layout_result->IntrinsicBlockSize();
      }

      // Constrain the intrinsic-size by the transferred min/max constraints.
      if (has_aspect_ratio) {
        const MinMaxSizes inline_min_max = ComputeMinMaxInlineSizes(
            flex_basis_space, child, border_padding_in_child_writing_mode,
            /* auto_min_length */ nullptr, MinMaxSizesFunc,
            TransferredSizesMode::kIgnore);
        const MinMaxSizes min_max = ComputeTransferredMinMaxBlockSizes(
            child_style.LogicalAspectRatio(), inline_min_max,
            border_padding_in_child_writing_mode,
            child_style.BoxSizingForAspectRatio());
        return min_max.ClampSizeToMinAndMax(intrinsic_size);
      }

      return intrinsic_size;
    };

    ipid_depth_log.FPrint(
        "为子元素 {} 定义 BlockSizeFunc "
        "闭包函数。这个函数用于按需计算子元素的高度，会处理替换元素、aspect-"
        "ratio、固有高度等复杂情况。函数的逻辑镜像了 "
        "BlockNode::ComputeMinMaxSizes 中的处理方式。",
        ipid::GetNodeStr(child));

    const Length& flex_basis = child_style.FlexBasis();
    if (is_column_ && flex_basis.MayHavePercentDependence()) {
      has_column_percent_flex_basis_ = true;
      ipid_depth_log.FPrint(
          "子元素 {} 的 flex-basis 为 {}，容器为列方向，且 flex-basis "
          "可能依赖百分比，因此设置 has_column_percent_flex_basis_ 标志为 "
          "true。这个标志用于后续的布局优化。",
          ipid::GetNodeStr(child), flex_basis);
    } else {
      ipid_depth_log.FPrint("子元素 {} 的 flex-basis 为 {}。",
                            ipid::GetNodeStr(child), flex_basis);
    }

    // This bool is set to true while calculating the base size, the flex-basis
    // is "content" based (e.g. dependent on the child's content).
    bool is_used_flex_basis_indefinite = false;

    ipid_depth_log.FPrint(
        "初始化 is_used_flex_basis_indefinite 标志为 "
        "false。这个标志在计算基础大小时，如果 flex-basis "
        "是基于内容的（例如依赖于子元素内容），会被设置为 true。");

    // An auto value for flex-basis says to defer to width or height.
    // Those might in turn have an auto value.  And in either case the
    // value might be calc-size(auto, ...).  Because of this, we might
    // need to handle resolving the length in the main axis twice.
    auto resolve_main_length = [&](const Length& used_flex_basis_length,
                                   const Length* auto_length) -> LayoutUnit {
      if (is_main_axis_inline_axis) {
        const LayoutUnit inline_size = ResolveMainInlineLength(
            flex_basis_space, child_style, border_padding_in_child_writing_mode,
            [&](SizeType type) -> MinMaxSizesResult {
              is_used_flex_basis_indefinite = true;
              return MinMaxSizesFunc(type);
            },
            used_flex_basis_length, auto_length);

        if (inline_size != kIndefiniteSize) {
          return inline_size;
        }

        // We weren't able to resolve the length (i.e. we were a unresolvable
        // %-age or similar), fallback to the max-content size.
        is_used_flex_basis_indefinite = true;
        return MinMaxSizesFunc(SizeType::kContent).sizes.max_size;
      }

      return ResolveMainBlockLength(
          flex_basis_space, child_style, border_padding_in_child_writing_mode,
          used_flex_basis_length, auto_length, [&](SizeType type) {
            is_used_flex_basis_indefinite = true;
            return BlockSizeFunc(type);
          });
    };

    ipid_depth_log.FPrint(
        "定义 resolve_main_length 闭包函数，用于解析子元素 {} "
        "主轴方向的长度。当 flex-basis 为 auto 时会延迟到 width 或 "
        "height，这些值可能也是 auto 或包含 calc-size(auto, "
        "...)，因此可能需要处理两次解析。根据主轴方向 "
        "is_main_axis_inline_axis={} 来决定是调用 ResolveMainInlineLength 还是 "
        "ResolveMainBlockLength。",
        ipid::GetNodeStr(child), ipid::btos(is_main_axis_inline_axis));

    const LayoutUnit flex_base_border_box = ([&]() -> LayoutUnit {
      std::optional<Length> auto_flex_basis_length;

      if (flex_basis.HasAuto()) {
        ipid_depth_log.FPrint(
            "子元素 {} 的 flex-basis 为 "
            "auto，需要根据主轴方向的指定尺寸（width 或 height）来确定实际值。",
            ipid::GetNodeStr(child));

        const Length& specified_length_in_main_axis =
            is_horizontal_flow_ ? child_style.Width() : child_style.Height();

        // 'auto' for items within a -webkit-box resolve as 'fit-content'.
        const Length& auto_size_length =
            (is_webkit_box_ &&
             (Style().BoxOrient() == EBoxOrient::kHorizontal ||
              Style().BoxAlign() != EBoxAlignment::kStretch))
                ? Length::FitContent()
                : Length::MaxContent();

        ipid_depth_log.FPrint(
            "主轴方向的指定尺寸为 {}。对于 -webkit-box "
            "兼容性：is_webkit_box_={}，如果是 webkit-box "
            "且为水平方向或非拉伸对齐，则 auto 解析为 fit-content；否则解析为 "
            "max-content。因此 auto_size_length 为 {}。",
            specified_length_in_main_axis, ipid::btos(is_webkit_box_),
            auto_size_length);

        LayoutUnit auto_flex_basis_size = resolve_main_length(
            specified_length_in_main_axis, &auto_size_length);
        if (child_style.BoxSizing() == EBoxSizing::kContentBox) {
          auto_flex_basis_size -= main_axis_border_padding;
          ipid_depth_log.FPrint(
              "子元素 {} 为 box-sizing: content-box，需要从解析出的尺寸 {} "
              "中减去主轴方向的 border+padding {}，得到 {}。",
              ipid::GetNodeStr(child),
              auto_flex_basis_size + main_axis_border_padding,
              main_axis_border_padding, auto_flex_basis_size);
        } else {
          ipid_depth_log.FPrint(
              "子元素 {} 为 box-sizing: border-box，解析出的尺寸 {} 直接使用。",
              ipid::GetNodeStr(child), auto_flex_basis_size);
        }
        DCHECK_GE(auto_flex_basis_size, LayoutUnit());
        auto_flex_basis_length = Length::Fixed(auto_flex_basis_size);

        ipid_depth_log.FPrint(
            "将计算出的 auto flex-basis 尺寸 {} 转换为 Length::Fixed({})。",
            auto_flex_basis_size, auto_flex_basis_size);
      }

      LayoutUnit main_size = resolve_main_length(
          flex_basis, base::OptionalToPtr(auto_flex_basis_length));

      ipid_depth_log.FPrint(
          "调用 resolve_main_length 解析最终的主轴尺寸：flex_basis={}, "
          "auto_length={}，解析结果为 {}。",
          flex_basis,
          auto_flex_basis_length.has_value()
              ? auto_flex_basis_length.value().ToString().Utf8()
              : "null",
          main_size);

      // Add the caption block-size only to sizes that are not content-based.
      if (!is_main_axis_inline_axis && !is_used_flex_basis_indefinite) {
        // 1. A table interprets forced block-size as the block-size of its
        //    captions and rows.
        // 2. The specified block-size of a table only applies to its rows.
        // 3. If the block-size resolved, add the caption block-size so that
        //    the forced block-size works correctly.
        if (const auto* table_child = DynamicTo<TableNode>(&child)) {
          LayoutUnit caption_block_size =
              table_child->ComputeCaptionBlockSize(child_space);
          main_size += caption_block_size;
          ipid_depth_log.FPrint(
              "子元素 {} 是表格，且主轴为高度方向，flex-basis "
              "不是基于内容的，需要添加表格标题的高度 {}。调整后的主轴尺寸为 "
              "{}。这是为了正确处理表格的强制高度。",
              ipid::GetNodeStr(child), caption_block_size, main_size);
        }
      }

      ipid_depth_log.FPrint("子元素 {} 的最终 flex-basis 边框盒尺寸为 {}。",
                            ipid::GetNodeStr(child), main_size);
      return main_size;
    })();

    // Spec calls this "flex base size"
    // https://www.w3.org/TR/css-flexbox-1/#algo-main-item
    // Blink's FlexibleBoxAlgorithm expects it to be content + scrollbar widths,
    // but no padding or border.
    DCHECK_GE(flex_base_border_box, main_axis_border_padding);
    const LayoutUnit base_content_size =
        flex_base_border_box - main_axis_border_padding;

    ipid_depth_log.FPrint(
        "根据 CSS Flexbox 规范，这被称为 'flex base size'。Blink 的 "
        "FlexibleBoxAlgorithm 期望它是内容+滚动条宽度，但不包括 padding 或 "
        "border。因此从边框盒尺寸 {} 减去主轴方向的 border+padding "
        "{}，得到基础内容尺寸 {}。",
        flex_base_border_box, main_axis_border_padding, base_content_size);

    std::optional<Length> auto_min_length;
    if (ShouldApplyAutoMinSize(child)) {
      ipid_depth_log.FPrint(
          "子元素 {} 符合应用自动最小尺寸的条件，开始计算 auto min size。这是 "
          "CSS Flexbox 规范中为防止 flex 项目过度收缩而设计的机制。",
          ipid::GetNodeStr(child));

      const LayoutUnit content_size_suggestion = ([&]() -> LayoutUnit {
        const LayoutUnit content_size =
            is_main_axis_inline_axis
                ? MinMaxSizesFunc(SizeType::kContent).sizes.min_size
                : BlockSizeFunc(SizeType::kContent);

        // For non-replaced elements with an aspect-ratio ensure the size
        // provided by the aspect-ratio encompasses the min-intrinsic size.
        if (!child.IsReplaced() && !child_style.AspectRatio().IsAuto()) {
          LayoutUnit min_intrinsic_size =
              is_main_axis_inline_axis
                  ? MinMaxSizesFunc(SizeType::kIntrinsic).sizes.min_size
                  : BlockSizeFunc(SizeType::kIntrinsic);
          LayoutUnit final_size = std::max(content_size, min_intrinsic_size);

          ipid_depth_log.FPrint(
              "子元素 {} 是非替换元素且有 aspect-ratio，需要确保 aspect-ratio "
              "提供的尺寸包含最小固有尺寸。content_size={}, "
              "min_intrinsic_size={}，取较大值 {} 作为内容尺寸建议。",
              ipid::GetNodeStr(child), content_size, min_intrinsic_size,
              final_size);
          return final_size;
        }

        ipid_depth_log.FPrint(
            "子元素 {} 的内容尺寸建议为 {}（来自 {} 方向的 {} 计算）。",
            ipid::GetNodeStr(child), content_size,
            is_main_axis_inline_axis ? "宽度" : "高度",
            is_main_axis_inline_axis ? "MinMaxSizesFunc(kContent).min_size"
                                     : "BlockSizeFunc(kContent)");
        return content_size;
      })();
      DCHECK_GE(content_size_suggestion, main_axis_border_padding);

      const LayoutUnit specified_size_suggestion = ([&]() -> LayoutUnit {
        const Length& specified_length_in_main_axis =
            is_horizontal_flow_ ? child_style.Width() : child_style.Height();
        if (specified_length_in_main_axis.HasAuto()) {
          ipid_depth_log.FPrint(
              "子元素 {} 主轴方向的指定尺寸为 auto，因此指定尺寸建议为 "
              "LayoutUnit::Max()（无限大）。",
              ipid::GetNodeStr(child));
          return LayoutUnit::Max();
        }
        const LayoutUnit resolved_size =
            is_main_axis_inline_axis
                ? ResolveMainInlineLength(
                      flex_basis_space, child_style,
                      border_padding_in_child_writing_mode, MinMaxSizesFunc,
                      specified_length_in_main_axis, /* auto_length */ nullptr)
                : ResolveMainBlockLength(flex_basis_space, child_style,
                                         border_padding_in_child_writing_mode,
                                         specified_length_in_main_axis,
                                         /* auto_length */ nullptr,
                                         BlockSizeFunc);

        // Coerce an indefinite size to LayoutUnit::Max().
        LayoutUnit final_size = resolved_size == kIndefiniteSize
                                    ? LayoutUnit::Max()
                                    : resolved_size;
        ipid_depth_log.FPrint(
            "子元素 {} 主轴方向的指定尺寸为 {}，解析后为 {}{}。",
            ipid::GetNodeStr(child), specified_length_in_main_axis,
            resolved_size,
            resolved_size == kIndefiniteSize
                ? "，由于是不确定值，转换为 LayoutUnit::Max()"
                : "");
        return final_size;
      })();

      LayoutUnit auto_min_size =
          std::min(specified_size_suggestion, content_size_suggestion);
      ipid_depth_log.FPrint(
          "计算 auto min size：取指定尺寸建议 {} 和内容尺寸建议 {} "
          "的较小值，得到 {}。",
          specified_size_suggestion == LayoutUnit::Max()
              ? "LayoutUnit::Max()"
              : std::to_string(specified_size_suggestion.ToDouble()) + "px",
          content_size_suggestion, auto_min_size);

      if (child_style.BoxSizing() == EBoxSizing::kContentBox) {
        auto_min_size -= main_axis_border_padding;
        ipid_depth_log.FPrint(
            "子元素 {} 为 box-sizing: content-box，需要从 auto min size {} "
            "中减去主轴方向的 border+padding {}，得到最终的 {}。",
            ipid::GetNodeStr(child), auto_min_size + main_axis_border_padding,
            main_axis_border_padding, auto_min_size);
      }
      DCHECK_GE(auto_min_size, LayoutUnit());
      auto_min_length = Length::Fixed(auto_min_size);

      ipid_depth_log.FPrint(
          "子元素 {} 的最终 auto_min_length 为 Length::Fixed({})。",
          ipid::GetNodeStr(child), auto_min_size);
    }

    MinMaxSizes min_max_sizes_in_main_axis_direction =
        is_main_axis_inline_axis
            ? ComputeMinMaxInlineSizes(
                  flex_basis_space, child, border_padding_in_child_writing_mode,
                  base::OptionalToPtr(auto_min_length), MinMaxSizesFunc,
                  TransferredSizesMode::kIgnore)
            : ComputeMinMaxBlockSizes(
                  flex_basis_space, child, border_padding_in_child_writing_mode,
                  base::OptionalToPtr(auto_min_length), BlockSizeFunc);

    min_max_sizes_in_main_axis_direction -= main_axis_border_padding;
    DCHECK_GE(min_max_sizes_in_main_axis_direction.min_size, LayoutUnit());
    DCHECK_GE(min_max_sizes_in_main_axis_direction.max_size, LayoutUnit());

    ipid_depth_log.FPrint(
        "计算子元素 {} 主轴方向的最小最大尺寸：根据主轴方向 "
        "is_main_axis_inline_axis={} 调用 {}。传入的 auto_min_length 为 "
        "{}。计算结果为 {}，然后减去主轴方向的 border+padding {}，最终结果为 "
        "{}。",
        ipid::GetNodeStr(child), ipid::btos(is_main_axis_inline_axis),
        is_main_axis_inline_axis ? "ComputeMinMaxInlineSizes"
                                 : "ComputeMinMaxBlockSizes",
        auto_min_length.has_value() ? auto_min_length.value().ToString().Utf8()
                                    : std::string("null"),
        ipid::GetMinMaxSizesString(
            MinMaxSizes{min_max_sizes_in_main_axis_direction.min_size +
                            main_axis_border_padding,
                        min_max_sizes_in_main_axis_direction.max_size +
                            main_axis_border_padding}),
        main_axis_border_padding,
        ipid::GetMinMaxSizesString(min_max_sizes_in_main_axis_direction));

    const BoxStrut initial_scrollbars = ComputeScrollbarsForNonAnonymous(child);

    ipid_depth_log.FPrint(
        "计算子元素 {} 的初始滚动条：{}。这用于确定滚动条对布局的影响。",
        ipid::GetNodeStr(child), ipid::GetBoxStrutString(initial_scrollbars));

    auto AspectRatioProvidesBlockMainSize = [&]() -> bool {
      if (is_main_axis_inline_axis) {
        return false;
      }
      if (child.IsReplaced()) {
        return false;
      }
      return !child_style.AspectRatio().IsAuto() &&
             InlineSizeFunc() != kIndefiniteSize;
    };

    ipid_depth_log.FPrint(
        "定义 AspectRatioProvidesBlockMainSize 闭包函数，用于判断 aspect-ratio "
        "是否为子元素 {} 提供高度方向的主轴尺寸。条件：1) 主轴不是宽度方向 "
        "{}，2) 不是替换元素 {}，3) 有 aspect-ratio 且宽度确定。",
        ipid::GetNodeStr(child),
        is_main_axis_inline_axis ? "false (不满足)" : "true",
        child.IsReplaced() ? "false (不满足)" : "true");

    // For flex-items whose main-axis is the block-axis we treat the initial
    // block-size as indefinite if:
    //  - The flex container has an indefinite main-size.
    //  - The used flex-basis is indefinite.
    //  - The aspect-ratio doesn't provide the main-size.
    //
    // See: // https://drafts.csswg.org/css-flexbox/#definite-sizes
    const bool is_aspect_ratio_provides_block_main_size =
        AspectRatioProvidesBlockMainSize();
    const bool is_initial_block_size_indefinite =
        is_column_ && !is_main_axis_inline_axis &&
        ChildAvailableSize().block_size == kIndefiniteSize &&
        is_used_flex_basis_indefinite &&
        !is_aspect_ratio_provides_block_main_size;

    ipid_depth_log.FPrint(
        "计算 is_initial_block_size_indefinite：对于主轴为高度方向的 flex "
        "项目，在以下条件下初始高度被视为不确定：1) flex 容器是列方向 {}，2) "
        "主轴不是宽度方向 {}，3) 子元素可用空间的高度不确定 {}，4) 使用的 "
        "flex-basis 不确定 {}，5) aspect-ratio 不提供主轴尺寸 "
        "{}。若上述所有条件为 "
        "true，则最终认为初始高度为不明确值。\n\n最终结果：{}。",
        ipid::btos(is_column_), ipid::btos(!is_main_axis_inline_axis),
        ipid::btos(ChildAvailableSize().block_size == kIndefiniteSize),
        ipid::btos(is_used_flex_basis_indefinite),
        ipid::btos(!is_aspect_ratio_provides_block_main_size),
        ipid::btos(is_initial_block_size_indefinite));

    const float flex_grow = child_style.ResolvedFlexGrow(Style());
    const float flex_shrink = child_style.ResolvedFlexShrink(Style());

    ipid_depth_log.FPrint(
        "获取子元素 {} 的 flex 属性：flex-grow = {}，flex-shrink = "
        "{}。这些值决定了元素在 flex 容器中如何增长和收缩。",
        ipid::GetNodeStr(child), flex_grow, flex_shrink);

    const auto container_writing_direction =
        GetConstraintSpace().GetWritingDirection();
    const auto baseline_writing_mode = DetermineBaselineWritingMode(
        container_writing_direction, child_writing_mode,
        /* is_parallel_context */ !is_column_);
    const auto baseline_group = DetermineBaselineGroup(
        container_writing_direction, baseline_writing_mode,
        /* is_parallel_context */ !is_column_,
        /* is_last_baseline */ alignment == ItemPosition::kLastBaseline,
        /* is_flipped */ is_wrap_reverse_);

    ipid_depth_log.FPrint(
        "计算子元素 {} 的基线对齐信息：容器书写方向为 {}，子元素书写模式为 "
        "{}，确定的基线书写模式为 {}，基线组为 "
        "{}。这些信息用于在交叉轴上进行基线对齐。",
        ipid::GetNodeStr(child), container_writing_direction,
        child_writing_mode, baseline_writing_mode,
        ipid::GetBaselineGroupString(baseline_group));

    flex_items_.emplace_back(
        child, item_index++, flex_grow, flex_shrink, base_content_size,
        min_max_sizes_in_main_axis_direction, main_axis_border_padding,
        max_content_contribution, physical_child_margins, initial_scrollbars,
        main_axis_auto_margin_count, alignment, baseline_writing_mode,
        baseline_group, is_initial_block_size_indefinite,
        is_used_flex_basis_indefinite, depends_on_min_max_sizes,
        is_horizontal_flow_);

    ipid_depth_log.FPrint(
        "完成子元素 {} 的 FlexItem 构造，添加到 flex_items_ 列表中。FlexItem "
        "包含了所有必要的布局信息：\n  子元素 index {}，\n  flex-grow {}，\n  "
        "flex-shrink "
        "{}，\n  基础内容尺寸 {}，\n  主轴最小最大尺寸 {}，\n  主轴 "
        "border+padding "
        "{}，\n  最大内容贡献 {}，\n  物理 margin {}，\n  初始滚动条 {}，\n  "
        "主轴 auto margin "
        "数量 {}，\n  对齐方式 {}，\n  基线组 {}，\n  初始高度是否不确定 "
        "{}，\n  使用的 flex-basis 是否不确定 {}，\n  是否依赖最小最大尺寸 "
        "{}，\n  是否水平流 {}。",
        ipid::GetNodeStr(child), item_index - 1, flex_grow, flex_shrink,
        base_content_size,
        ipid::GetMinMaxSizesString(min_max_sizes_in_main_axis_direction),
        main_axis_border_padding,
        max_content_contribution.has_value()
            ? std::to_string(max_content_contribution.value().ToDouble()) + "px"
            : "nullopt",
        ipid::GetBoxStrutString(physical_child_margins),
        ipid::GetBoxStrutString(initial_scrollbars),
        main_axis_auto_margin_count, ipid::GetItemPositionString(alignment),
        ipid::GetBaselineGroupString(baseline_group),
        ipid::btos(is_initial_block_size_indefinite),
        ipid::btos(is_used_flex_basis_indefinite),
        ipid::btos(depends_on_min_max_sizes), ipid::btos(is_horizontal_flow_));
  }

  ipid_depth_log.FPrint(
      "完成所有子元素的处理，共构造了 {} 个 "
      "FlexItem。ConstructAndAppendFlexItems 函数执行完毕。",
      flex_items_.size());
}

const LayoutResult* FlexLayoutAlgorithm::Layout() {
  auto* result = LayoutInternal();
  switch (result->Status()) {
    case LayoutResult::kNeedsEarlierBreak:
      // If we found a good break somewhere inside this block, re-layout and
      // break at that location.
      DCHECK(result->GetEarlyBreak());
      return RelayoutAndBreakEarlier<FlexLayoutAlgorithm>(
          *result->GetEarlyBreak(), &column_early_breaks_);
    case LayoutResult::kNeedsRelayoutWithNoChildScrollbarChanges:
      DCHECK(!ignore_child_scrollbar_changes_);
      return Relayout<FlexLayoutAlgorithm>(
          kRelayoutIgnoringChildScrollbarChanges);
    case LayoutResult::kDisableFragmentation:
      DCHECK(GetConstraintSpace().HasBlockFragmentation());
      return RelayoutWithoutFragmentation<FlexLayoutAlgorithm>();
    case LayoutResult::kNeedsRelayoutWithRowCrossSizeChanges:
      return RelayoutWithNewRowSizes();
    default:
      return result;
  }
}

const LayoutResult* FlexLayoutAlgorithm::LayoutInternal() {
  IpidDepthLog ipid_depth_log("FlexLayoutAlgorithm::LayoutInternal");

  ipid_depth_log.FPrint(
      "开始执行 flex 容器的内部布局。这是 flex 布局算法的核心函数，负责完成从 "
      "flex 项目放置到最终尺寸计算的整个流程。");
  // Freezing the scrollbars for the sub-tree shouldn't be strictly necessary,
  // but we do this just in case we trigger an unstable layout.
  std::optional<PaintLayerScrollableArea::FreezeScrollbarsScope>
      freeze_scrollbars;
  if (ignore_child_scrollbar_changes_) {
    ipid_depth_log.FPrint(
        "检测到需要忽略子元素滚动条变化，冻结滚动条状态以防止不稳定的布局。");
    freeze_scrollbars.emplace();
  }

  PaintLayerScrollableArea::DelayScrollOffsetClampScope delay_clamp_scope;

  Vector<EBreakBetween> row_break_between_outputs;
  FlexLineVector flex_lines;
  HeapVector<Member<LayoutBox>> oof_children;
  FlexBreakTokenData::FlexBreakBeforeRow break_before_row =
      FlexBreakTokenData::kNotBreakBeforeRow;
  LayoutUnit total_intrinsic_block_size;

  ipid_depth_log.FPrint(
      "初始化 flex 布局相关变量：\n"
      "- row_break_between_outputs: 用于存储行间分页信息\n"
      "- flex_lines: 用于存储 flex 行数据\n"
      "- oof_children: 用于存储绝对定位子元素\n"
      "- break_before_row: 行前分页状态，当前为 {}\n"
      "- total_intrinsic_block_size: flex 容器的总固有高度",
      ipid::GetFlexBreakBeforeRowString(static_cast<int>(break_before_row)));

  ClearCollectionScope<FlexLineVector> scope(&flex_lines);

  ipid_depth_log.FPrint(
      "判断当前布局是否为分片布局的继续。如果是，则从 break token 中恢复状态；否则重新开始 flex 项目的放置。");

  if (IsBreakInside(GetBreakToken())) {
    const auto* flex_data =
        To<FlexBreakTokenData>(GetBreakToken()->TokenData());
    total_intrinsic_block_size = flex_data->intrinsic_block_size;
    flex_lines = flex_data->flex_lines;
    row_break_between_outputs = flex_data->row_break_between;
    break_before_row = flex_data->break_before_row;
    oof_children = flex_data->oof_children;
    
    ipid_depth_log.FPrint(
        "[分片恢复] 从 break token 中恢复之前的布局状态：\n"
        "- 总固有高度: {}\n"
        "- flex 行数: {} 行\n"
        "- 行间分页数: {} 个\n"
        "- 行前分页状态: {}\n"
        "- 绝对定位子元素数: {} 个",
        total_intrinsic_block_size,
        flex_lines.size(),
        row_break_between_outputs.size(),
        ipid::GetFlexBreakBeforeRowString(static_cast<int>(break_before_row)),
        oof_children.size());
  } else {
    ipid_depth_log.FPrint(
        "[非分片模式] 开始执行 flex 项目放置。调用 PlaceFlexItems 函数，"
        "使用布局阶段 Phase::kLayout 进行真正的布局。");
    PlaceFlexItems(Phase::kLayout, &flex_lines, &oof_children,
                   &total_intrinsic_block_size);
  }

  ipid_depth_log.FPrint(
      "开始计算 flex 容器的最终高度。调用 ComputeBlockSizeForFragment 函数，"
      "传入总固有高度 {} 和容器宽度 {} 进行计算。",
      total_intrinsic_block_size, container_builder_.InlineSize());
      
  total_block_size_ = ComputeBlockSizeForFragment(
      GetConstraintSpace(), Node(), BorderPadding(), total_intrinsic_block_size,
      container_builder_.InlineSize());
      
  ipid_depth_log.FPrint(
      "flex 容器最终高度计算结果: {}",
      total_block_size_);

  if (!IsBreakInside(GetBreakToken())) {
    ipid_depth_log.FPrint(
        "[非分片模式] 开始应用 flex 布局的后处理步骤：\n"
        "1. 应用 flex 方向反转（ApplyReversals）\n"
        "2. 给予 flex 项目最终位置和尺寸（GiveItemsFinalPositionAndSize）");
        
    ApplyReversals(&flex_lines);
    
    LayoutResult::EStatus status =
        GiveItemsFinalPositionAndSize(&flex_lines, &row_break_between_outputs);
    if (status != LayoutResult::kSuccess) {
      ipid_depth_log.FPrint(
          "[布局错误] GiveItemsFinalPositionAndSize 返回了非成功状态: {}\n"
          "终止布局并返回错误。",
          ipid::GetLayoutResultStatusString(static_cast<int>(status)));
      return container_builder_.Abort(status);
    }
    
    ipid_depth_log.FPrint(
        "[成功] flex 项目的后处理步骤完成，状态: {}",
        ipid::GetLayoutResultStatusString(static_cast<int>(status)));
  }

  LayoutUnit previously_consumed_block_size;
  if (GetBreakToken()) [[unlikely]] {
    previously_consumed_block_size = GetBreakToken()->ConsumedBlockSize();
    ipid_depth_log.FPrint(
        "[分片情况] 检测到 break token，之前的分片已消耗的高度: {}",
        previously_consumed_block_size);
  }

  intrinsic_block_size_ = BorderScrollbarPadding().block_start;
  LayoutUnit block_size;
  
  ipid_depth_log.FPrint(
      "开始计算最终的 intrinsic_block_size_ 和 block_size。\n"
      "初始 intrinsic_block_size_ 为 border+scrollbar+padding 的上方值: {}",
      intrinsic_block_size_);
  if (InvolvedInBlockFragmentation(container_builder_)) [[unlikely]] {
    ipid_depth_log.FPrint(
        "[分片处理] 当前布局涉及块分片（分页）处理。");
        
    const bool use_empty_line_block_size =
        flex_lines.empty() && Node().HasLineIfEmpty();
    if (use_empty_line_block_size) {
      ipid_depth_log.FPrint(
          "检测到 flex 行为空且该元素有 HasLineIfEmpty 标记，需要调整 intrinsic_block_size_。");
      intrinsic_block_size_ =
          (total_intrinsic_block_size - BorderScrollbarPadding().block_end -
           previously_consumed_block_size)
              .ClampNegativeToZero();
      ipid_depth_log.FPrint(
          "调整后的 intrinsic_block_size_: {}",
          intrinsic_block_size_);
    }

    ipid_depth_log.FPrint(
        "开始为分片情况计算 flex 项目的最终位置和尺寸。");
        
    LayoutResult::EStatus status =
        GiveItemsFinalPositionAndSizeForFragmentation(
            &flex_lines, &row_break_between_outputs, &break_before_row,
            &total_intrinsic_block_size);
    if (status != LayoutResult::kSuccess) {
      ipid_depth_log.FPrint(
          "[分片错误] GiveItemsFinalPositionAndSizeForFragmentation 返回错误状态: {}\n"
          "终止布局并返回错误。",
          ipid::GetLayoutResultStatusString(static_cast<int>(status)));
      return container_builder_.Abort(status);
    }

    intrinsic_block_size_ = ClampIntrinsicBlockSize(
        GetConstraintSpace(), Node(), GetBreakToken(), BorderScrollbarPadding(),
        intrinsic_block_size_ + BorderScrollbarPadding().block_end);

    ipid_depth_log.FPrint(
        "分片下的 intrinsic_block_size_ 经 ClampIntrinsicBlockSize 调整后: {}",
        intrinsic_block_size_);

    block_size = ComputeBlockSizeForFragment(
        GetConstraintSpace(), Node(), BorderPadding(),
        previously_consumed_block_size + intrinsic_block_size_,
        container_builder_.InlineSize());
        
    ipid_depth_log.FPrint(
        "[分片] 最终计算的 block_size: {}",
        block_size);
  } else {
    ipid_depth_log.FPrint(
        "[非分片] 使用正常的值：\n"
        "- intrinsic_block_size_ = total_intrinsic_block_size: {}\n"
        "- block_size = total_block_size_: {}",
        total_intrinsic_block_size, total_block_size_);
    intrinsic_block_size_ = total_intrinsic_block_size;
    block_size = total_block_size_;
  }

  ipid_depth_log.FPrint(
      "将计算结果设置到 container_builder_ 中：\n"
      "- IntrinsicBlockSize: {}\n"
      "- FragmentsTotalBlockSize: {}",
      intrinsic_block_size_, block_size);
      
  container_builder_.SetIntrinsicBlockSize(intrinsic_block_size_);
  container_builder_.SetFragmentsTotalBlockSize(block_size);

  if (has_column_percent_flex_basis_) {
    ipid_depth_log.FPrint(
        "检测到 flex 项目使用了百分比的 flex-basis，标记容器依赖百分比高度。");
    container_builder_.SetHasDescendantThatDependsOnPercentageBlockSize(true);
  }
  if (layout_info_for_devtools_) [[unlikely]] {
    ipid_depth_log.FPrint(
        "传输 flex 布局信息给开发者工具。");
    container_builder_.TransferFlexLayoutData(
        std::move(layout_info_for_devtools_));
  }

  if (InvolvedInBlockFragmentation(container_builder_)) [[unlikely]] {
    ipid_depth_log.FPrint(
        "[分片终结] 开始终结分片处理。");
        
    BreakStatus break_status = FinishFragmentation(&container_builder_);
    
    ipid_depth_log.FPrint(
        "FinishFragmentation 返回状态: {}",
        ipid::GetBreakStatusString(static_cast<int>(break_status)));
        
    if (break_status != BreakStatus::kContinue) {
      if (break_status == BreakStatus::kNeedsEarlierBreak) {
        ipid_depth_log.FPrint(
            "[分片错误] 需要更早的分页点，返回 kNeedsEarlierBreak。");
        return container_builder_.Abort(LayoutResult::kNeedsEarlierBreak);
      }
      DCHECK_EQ(break_status, BreakStatus::kDisableFragmentation);
      ipid_depth_log.FPrint(
          "[分片错误] 禁用分片，返回 kDisableFragmentation。");
      return container_builder_.Abort(LayoutResult::kDisableFragmentation);
    }
    
    ipid_depth_log.FPrint(
        "[分片成功] 分片处理正常继续。");
  } else {
#if DCHECK_IS_ON()
    // If we're not participating in a fragmentation context, no block
    // fragmentation related fields should have been set.
    ipid_depth_log.FPrint(
        "[调试检查] 非分片模式下，检查未设置分片相关字段。");
    container_builder_.CheckNoBlockFragmentation();
#endif
  }

  ipid_depth_log.FPrint(
      "处理 reading flow nodes 和绝对定位元素：\n"
      "- 设置 reading flow nodes\n"
      "- 处理 {} 个绝对定位子元素",
      oof_children.size());
      
  SetReadingFlowNodes(flex_lines);
  HandleOutOfFlowPositionedItems(total_intrinsic_block_size, oof_children);

  // For rows, the break-before of the first row and the break-after of the
  // last row are propagated to the container. For columns, treat the set
  // of columns as a single row and propagate the combined break-before rules
  // for the first items in each column and break-after rules for last items in
  // each column.
  if (GetConstraintSpace().ShouldPropagateChildBreakValues()) {
    DCHECK(!row_break_between_outputs.empty());
    ipid_depth_log.FPrint(
        "[分页传播] 将子元素的分页值传播到容器：\n"
        "- InitialBreakBefore: {}\n"
        "- PreviousBreakAfter: {}",
        row_break_between_outputs.front(), row_break_between_outputs.back());
    container_builder_.SetInitialBreakBefore(row_break_between_outputs.front());
    container_builder_.SetPreviousBreakAfter(row_break_between_outputs.back());
  }

  if (GetConstraintSpace().HasBlockFragmentation()) {
    ipid_depth_log.FPrint(
        "[分片数据] 为分片情况设置 break token data：\n"
        "- {} 行 flex 数据\n"
        "- {} 个行间分页\n"
        "- {} 个绝对定位子元素\n"
        "- 总固有高度: {}\n"
        "- 行前分页状态: {}",
        flex_lines.size(),
        row_break_between_outputs.size(), 
        oof_children.size(),
        total_intrinsic_block_size,
        ipid::GetFlexBreakBeforeRowString(static_cast<int>(break_before_row)));
        
    container_builder_.SetBreakTokenData(
        MakeGarbageCollected<FlexBreakTokenData>(
            container_builder_.GetBreakTokenData(), flex_lines,
            row_break_between_outputs, oof_children, total_intrinsic_block_size,
            break_before_row));
  }

  // Un-freeze descendant scrollbars before we run the OOF layout part.
  ipid_depth_log.FPrint(
      "解冻后代滚动条，准备处理 OOF（Out-Of-Flow）布局部分。");
  freeze_scrollbars.reset();

  ipid_depth_log.FPrint(
      "处理 OOF 和特殊后代，然后生成最终的 BoxFragment。");
  container_builder_.HandleOofsAndSpecialDescendants();

  ipid_depth_log.FPrint(
      "flex 布局完成，返回最终的 LayoutResult。");
  return container_builder_.ToBoxFragment();
}

void FlexLayoutAlgorithm::PlaceFlexItems(
    Phase phase,
    FlexLineVector* flex_lines,
    HeapVector<Member<LayoutBox>>* oof_children,
    LayoutUnit* total_intrinsic_block_size_out) {
  IpidDepthLog ipid_depth_log("FlexLayoutAlgorithm::PlaceFlexItems");

  ipid_depth_log.FPrint(
      "开始 flex 项目放置阶段。当前的布局阶段为 {}，这决定了如何处理 flex "
      "项目的构建和布局：如果是 kLayout 则进行真正的布局、如果是 "
      "kRowIntrinsicSize 则计算行 flex 容器的固有宽度、如果是 "
      "kColumnWrapIntrinsicSize 则计算列换行 flex 容器的固有宽度。",
      ipid::GetFlexLayoutAlgorithmPhaseString(static_cast<int>(phase)));

  DCHECK(oof_children || phase != Phase::kLayout);

  ipid_depth_log.FPrint(
      "开始构建所有 flex "
      "项目的基础信息。ConstructAndAppendFlexItems "
      "函数会遍历所有子元素，计算每个元素的 flex base "
      "size、min/max sizes、对齐方式等属性，为后续的换行和弹性计算做准备。");
  ConstructAndAppendFlexItems(phase, oof_children);

  ipid_depth_log.FPrint(
      "ConstructAndAppendFlexItems 完成，共构建了 {} 个 flex 项目。",
      flex_items_.size());

  ipid_depth_log.FPrint(
      "开始计算 flex "
      "容器在主轴方向的可用空间。通过调用 "
      "MainAxisContentExtent(LayoutUnit::Max()) "
      "来获取主轴内容区域的最大可用尺寸，这个值将用于确定是否需要换行。");
  const LayoutUnit line_break_size = MainAxisContentExtent(LayoutUnit::Max());

  ipid_depth_log.FPrint(
      "主轴方向的可用空间为 {}px。现在调用 BreakFlexItemsIntoLines "
      "函数将所有 flex "
      "项目按照换行规则分组到不同的行中。换行依据包括：主轴可用空间、项目间间距"
      " "
      "{}px、是否允许多行换行 {}、以及平衡最小行数 {}。",
      line_break_size, gap_between_items_, ipid::btos(is_multi_line_),
      balance_min_line_count_.has_value()
          ? std::to_string(*balance_min_line_count_)
          : "none");

  const FlexLineBreakerResult result = BreakFlexItemsIntoLines(
      base::span(flex_items_), line_break_size, gap_between_items_,
      is_multi_line_, balance_min_line_count_);

  ipid_depth_log.FPrint(
      "BreakFlexItemsIntoLines 完成，将所有项目分成了 {} "
      "行。所有行中假定主轴尺寸的最大总和为 {}px，这个值用于确定列 flex "
      "容器的固有高度。",
      result.flex_lines.size(), result.max_sum_hypothetical_main_size);

  // For column flexboxes we can now determine the intrinsic block-size, which
  // we use to flex all the lines to.
  ipid_depth_log.FPrint(
      "对于列 flex "
      "容器，我们现在可以确定固有高度，它等于所有行中假定主轴尺寸的最大总和。"
      "调用 MainAxisContentExtent({}) 来计算主轴内容区域的实际尺寸，"
      "这个值将作为所有行进行弹性伸缩的目标空间。",
      result.max_sum_hypothetical_main_size);

  const LayoutUnit main_axis_inner_size =
      MainAxisContentExtent(result.max_sum_hypothetical_main_size);

  ipid_depth_log.FPrint(
      "计算得到的主轴内容区域尺寸为 "
      "{}px，这个值将用于后续每一行的弹性伸缩计算。",
      main_axis_inner_size);

  // If we are a single line, and have a definite cross-size, the line
  // cross-size will be the container cross-size.
  ipid_depth_log.FPrint(
      "开始计算确定的行交叉轴尺寸。如果当前是单行 flex "
      "容器且交叉轴尺寸是确定的，那么行的交叉轴尺寸就等于容器的交叉轴尺寸。"
      "当前容器是否允许多行：{}。",
      ipid::btos(is_multi_line_));

  const std::optional<LayoutUnit> definite_line_cross_size =
      ([&]() -> std::optional<LayoutUnit> {
        if (is_multi_line_) {
          ipid_depth_log.FPrint(
              "由于当前容器允许多行，每行的交叉轴尺寸需要根据该行内容动态计算，"
              "因此确定的行交叉轴尺寸为空。");
          return std::nullopt;
        }

        const LayoutUnit cross_available_size =
            is_column_ ? ChildAvailableSize().inline_size
                       : ChildAvailableSize().block_size;

        ipid_depth_log.FPrint(
            "当前是单行容器，交叉轴方向的可用尺寸为 "
            "{}px（{}"
            "）。如果该值不确定或存在特殊情况，则行交叉轴尺寸将设为空。",
            cross_available_size, is_column_ ? "宽度方向" : "高度方向");

        if (cross_available_size == kIndefiniteSize) {
          ipid_depth_log.FPrint(
              "交叉轴可用尺寸为不确定值 (-1)，因此确定的行交叉轴尺寸为空。");
          return std::nullopt;
        }

        const auto& style = Style();
        if (!is_column_) {
          // Treat the block-size as indefinite if we need to apply the
          // automatic-minimum size for aspect-ratio.
          // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-minimum
          if (!style.AspectRatio().IsAuto() && !style.IsScrollContainer() &&
              style.LogicalMinHeight().HasAuto()) {
            ipid_depth_log.FPrint(
                "当前容器有 aspect-ratio 且 min-height 为 auto，根据 CSS "
                "规范需要应用自动最小尺寸，因此将高度视为不确定，确定的行交叉轴"
                "尺寸为空。");
            return std::nullopt;
          }
          // Similarly if we have a content-based min/max block-size treat it
          // as indefinite.
          // NOTE: This behaviour isn't in the specification.
          // https://github.com/w3c/csswg-drafts/issues/12123
          if (style.LogicalMinHeight().HasContentOrIntrinsic() ||
              style.LogicalMaxHeight().HasContentOrIntrinsic()) {
            ipid_depth_log.FPrint(
                "当前容器的 min-height 或 max-height "
                "使用了基于内容的值（如 min-content、max-content），"
                "因此将高度视为不确定，确定的行交叉轴尺寸为空。");
            return std::nullopt;
          }
        }

        ipid_depth_log.FPrint(
            "经过检查，交叉轴可用尺寸 {}px "
            "是确定且有效的，因此使用该值作为确定的行交叉轴尺寸。",
            cross_available_size);
        return cross_available_size;
      })();

  if (definite_line_cross_size.has_value()) {
    ipid_depth_log.FPrint(
        "最终确定的行交叉轴尺寸为 "
        "{}px，这意味着所有行都将使用这个固定的交叉轴尺寸。",
        *definite_line_cross_size);
  } else {
    ipid_depth_log.FPrint(
        "最终确定的行交叉轴尺寸为空，这意味着每行的交叉轴尺寸需要根据该行内的项"
        "目动态计算。");
  }

  LayoutUnit sum_line_cross_size;

  ipid_depth_log.FPrint(
      "开始处理每一行的 flex "
      "项目。总共需要处理 {} "
      "行，对每一行都会执行弹性伸缩计算、交叉轴尺寸计算、基线对齐等操作。",
      result.flex_lines.size());

  flex_lines->reserve(result.flex_lines.size());
  for (wtf_size_t line_index = 0; line_index < result.flex_lines.size();
       ++line_index) {
    auto& line = result.flex_lines[line_index];

    ipid_depth_log.FPrint(
        "正在处理第 {} 行，该行包含 {} 个 flex "
        "项目。行的假定主轴尺寸总和为 {}px，flex base size 总和为 {}px。",
        line_index + 1, line.line_items.size(), line.sum_hypothetical_main_size,
        line.sum_flex_base_size);
    // Flex the items.
    ipid_depth_log.FPrint(
        "开始对第 {} 行进行弹性伸缩计算。LineFlexer "
        "会根据每个项目的 flex-grow、flex-shrink 属性以及可用的主轴空间 "
        "{}px，计算每个项目的最终主轴尺寸。",
        line_index + 1, main_axis_inner_size);

    LineFlexer(base::span(line.line_items), line.sum_hypothetical_main_size,
               line.sum_flex_base_size, main_axis_inner_size)
        .Run();

    ipid_depth_log.FPrint("第 {} 行的弹性伸缩计算完成。", line_index + 1);

    Vector<wtf_size_t> item_indices;
    item_indices.ReserveInitialCapacity(line.line_items.size());

    LayoutUnit main_axis_free_space =
        main_axis_inner_size -
        (line.line_items.size() - 1) * gap_between_items_;

    ipid_depth_log.FPrint(
        "计算第 {} 行的主轴剩余空间。主轴内容区域尺寸 {}px 减去 {} "
        "个项目间间距（每个 {}px），得到初始剩余空间 {}px。",
        line_index + 1, main_axis_inner_size, line.line_items.size() - 1,
        gap_between_items_, main_axis_free_space);

    LayoutUnit line_cross_size;
    LayoutUnit max_major_ascent = LayoutUnit::Min();
    LayoutUnit max_minor_ascent = LayoutUnit::Min();
    LayoutUnit max_major_descent = LayoutUnit::Min();
    LayoutUnit max_minor_descent = LayoutUnit::Min();
    unsigned main_axis_auto_margin_count = 0;

    ipid_depth_log.FPrint(
        "开始遍历第 {} 行的每个项目，计算交叉轴尺寸和基线对齐信息。",
        line_index + 1);

    for (wtf_size_t i = 0; i < line.line_items.size(); ++i) {
      FlexItem& flex_item = line.line_items[i];

      ipid_depth_log.FPrint(
          "处理第 {} 行中的第 {} 个项目：{}。该项目弹性伸缩后的主轴尺寸（包含 "
          "margin）为 {}px，主轴方向有 {} 个 auto margin。",
          line_index + 1, i + 1, ipid::GetNodeStr(flex_item.block_node),
          flex_item.FlexedMarginBoxSize(),
          flex_item.main_axis_auto_margin_count);

      item_indices.push_back(flex_item.item_index);
      main_axis_free_space -= flex_item.FlexedMarginBoxSize();
      main_axis_auto_margin_count += flex_item.main_axis_auto_margin_count;

      const bool has_baseline_alignment =
          flex_item.alignment == ItemPosition::kBaseline ||
          flex_item.alignment == ItemPosition::kLastBaseline;

      ipid_depth_log.FPrint(
          "项目的对齐方式为 {}，{} 基线对齐。当前行剩余主轴空间为 {}px，"
          "累计 auto margin 数量为 {}。",
          ipid::GetItemPositionString(flex_item.alignment),
          has_baseline_alignment ? "需要" : "不需要", main_axis_free_space,
          main_axis_auto_margin_count);

      // If we don't need to compute the line cross-size or don't have anything
      // baseline aligned - we can skip the rest of this loop.
      if (!has_baseline_alignment && definite_line_cross_size) {
        ipid_depth_log.FPrint(
            "项目不需要基线对齐且行交叉轴尺寸已确定为 {}px，"
            "跳过该项目的交叉轴尺寸计算。",
            *definite_line_cross_size);
        continue;
      }

      const BlockNode& node = flex_item.block_node;

      ipid_depth_log.FPrint(
          "为项目 {} 构建布局约束空间。项目的对齐方式为 {}，"
          "初始高度是否不确定：{}，弹性伸缩后的主轴 border-box 尺寸为 {}px。",
          ipid::GetNodeStr(node),
          ipid::GetItemPositionString(flex_item.alignment),
          ipid::btos(flex_item.is_initial_block_size_indefinite),
          flex_item.FlexedBorderBoxSize());

      const ConstraintSpace space = BuildSpaceForLayout(
          node, flex_item.alignment, flex_item.is_initial_block_size_indefinite,
          flex_item.max_content_contribution, flex_item.FlexedBorderBoxSize());

      ipid_depth_log.FPrint("为项目 {} 构建的约束空间：{}",
                            ipid::GetNodeStr(node),
                            ipid::GetConstraintSpaceString(space));

      const LayoutResult* layout_result = nullptr;

      ipid_depth_log.FPrint(
          "开始计算项目 {} 的交叉轴尺寸。这个计算会根据项目类型（替换元素 vs "
          "普通元素）、主轴方向、以及当前布局阶段采用不同的计算策略。",
          ipid::GetNodeStr(node));

      const LayoutUnit cross_axis_size = ([&]() {
        const auto& item_style = node.Style();
        const BoxStrut border_padding =
            ComputeBorders(space, node) + ComputePadding(space, item_style);
        const bool is_main_axis_inline_axis =
            IsHorizontalWritingMode(item_style.GetWritingMode()) ==
            is_horizontal_flow_;

        ipid_depth_log.FPrint(
            "项目 {} 的 border+padding: {}，主轴是否为宽度方向：{}。",
            ipid::GetNodeStr(node), ipid::GetBoxStrutString(border_padding),
            ipid::btos(is_main_axis_inline_axis));

        if (node.IsReplaced()) {
          ipid_depth_log.FPrint(
              "项目 {} 是替换元素（如 img、video "
              "等），调用 ComputeReplacedSize 计算其固有尺寸。",
              ipid::GetNodeStr(node));

          const LogicalSize replaced_size =
              ComputeReplacedSize(node, space, border_padding);

          const LayoutUnit cross_size = is_main_axis_inline_axis
                                            ? replaced_size.block_size
                                            : replaced_size.inline_size;

          ipid_depth_log.FPrint(
              "替换元素 {} 的固有尺寸为 {}，交叉轴尺寸为 {}px。",
              ipid::GetNodeStr(node), ipid::GetLogicalSizeString(replaced_size),
              cross_size);

          return cross_size;
        }

        if (!is_main_axis_inline_axis) {
          ipid_depth_log.FPrint(
              "项目 {} 的主轴不是宽度方向，因此交叉轴尺寸就是项目的宽度。"
              "调用 ComputeInlineSizeForFragment 计算宽度。",
              ipid::GetNodeStr(node));

          const LayoutUnit cross_size =
              ComputeInlineSizeForFragment(space, node, border_padding);

          ipid_depth_log.FPrint("项目 {} 的交叉轴尺寸（宽度）为 {}px。",
                                ipid::GetNodeStr(node), cross_size);

          return cross_size;
        }

        if (phase == Phase::kColumnWrapIntrinsicSize) {
          ipid_depth_log.FPrint(
              "当前阶段为 kColumnWrapIntrinsicSize，直接使用项目 {} "
              "预先计算好的 max-content 贡献值 {}px 作为交叉轴尺寸。",
              ipid::GetNodeStr(node), *flex_item.max_content_contribution);
          return *flex_item.max_content_contribution;
        }

        ipid_depth_log.FPrint(
            "项目 {} "
            "的主轴是宽度方向且不是替换元素，需要通过布局来确定其交叉轴尺寸（高"
            "度）。",
            ipid::GetNodeStr(node));

        std::optional<DisableLayoutSideEffectsScope> disable_side_effects;
        if (phase != Phase::kLayout && !node.GetLayoutBox()->NeedsLayout()) {
          ipid_depth_log.FPrint(
              "当前不是 kLayout 阶段且项目 {} 不需要布局，启用 "
              "DisableLayoutSideEffectsScope 来避免布局副作用。",
              ipid::GetNodeStr(node));
          disable_side_effects.emplace();
        }

        layout_result = node.Layout(space);
        const PhysicalSize size = layout_result->GetPhysicalFragment().Size();
        const LayoutUnit cross_size =
            is_horizontal_flow_ ? size.height : size.width;

        ipid_depth_log.FPrint(
            "项目 {} 布局完成，物理尺寸为 {}x{}px，交叉轴尺寸为 {}px。",
            ipid::GetNodeStr(node), size.width, size.height, cross_size);

        return cross_size;
      })();

      // Calculate the size used to determine the line cross-axis size.
      //
      // Typically this is just the cross-axis size, however if we are baseline
      // aligned we need to track the baseline(s) max ascent/descent, and use
      // the "baseline" size instead.
      LayoutUnit cross_axis_margin_size =
          cross_axis_size + flex_item.CrossAxisMarginExtent();

      ipid_depth_log.FPrint(
          "项目 {} 的交叉轴尺寸为 {}px，加上交叉轴 margin {}px "
          "后，用于确定行交叉轴尺寸的数值为 {}px。",
          ipid::GetNodeStr(node), cross_axis_size,
          flex_item.CrossAxisMarginExtent(), cross_axis_margin_size);

      if (has_baseline_alignment) {
        ipid_depth_log.FPrint(
            "项目 {} 需要基线对齐（{}），开始计算基线上升高度和下降高度。",
            ipid::GetNodeStr(node),
            ipid::GetItemPositionString(flex_item.alignment));
        // When computing `cross_axis_size` we'll run layout when the
        // flex-item's cross-size is its block-size, and we'll have a
        // layout-result here to pull the baseline from. In all other
        // cases we can avoid layout and just synthesize the baseline.
        const LayoutUnit ascent =
            layout_result
                ? BaselineAscent(flex_item,
                                 To<PhysicalBoxFragment>(
                                     layout_result->GetPhysicalFragment()))
                : SynthesizedBaselineAscent(flex_item, cross_axis_size);

        ipid_depth_log.FPrint("项目 {} 的基线上升高度为 {}px（{}获得）。",
                              ipid::GetNodeStr(node), ascent,
                              layout_result ? "通过布局结果" : "通过合成计算");

        const LayoutUnit descent = cross_axis_margin_size - ascent;

        ipid_depth_log.FPrint(
            "项目 {} 的基线下降高度为 {}px，属于 {} 基线组。",
            ipid::GetNodeStr(node), descent,
            ipid::GetBaselineGroupString(flex_item.baseline_group));

        if (flex_item.baseline_group == BaselineGroup::kMajor) {
          max_major_ascent = std::max(max_major_ascent, ascent);
          max_major_descent = std::max(max_major_descent, descent);
          cross_axis_margin_size = max_major_ascent + max_major_descent;

          ipid_depth_log.FPrint(
              "更新主要基线组的最大上升高度为 {}px，最大下降高度为 {}px，"
              "该项目对应的交叉轴尺寸更新为 {}px。",
              max_major_ascent, max_major_descent, cross_axis_margin_size);
        } else {
          max_minor_ascent = std::max(max_minor_ascent, ascent);
          max_minor_descent = std::max(max_minor_descent, descent);
          cross_axis_margin_size = max_minor_ascent + max_minor_descent;

          ipid_depth_log.FPrint(
              "更新次要基线组的最大上升高度为 {}px，最大下降高度为 {}px，"
              "该项目对应的交叉轴尺寸更新为 {}px。",
              max_minor_ascent, max_minor_descent, cross_axis_margin_size);
        }
      }
      line_cross_size = std::max(line_cross_size, cross_axis_margin_size);

      ipid_depth_log.FPrint(
          "处理完项目 {}，当前行的交叉轴尺寸更新为 "
          "{}px（取所有项目的最大值）。",
          ipid::GetNodeStr(node), line_cross_size);
    }

    ipid_depth_log.FPrint(
        "第 {} 行的所有项目处理完成。行的最终主轴剩余空间为 {}px，"
        "交叉轴尺寸为 {}px，主要基线上升/下降高度为 {}px/{}px，"
        "次要基线上升/下降高度为 {}px/{}px，主轴 auto margin 总数为 {}。",
        line_index + 1, main_axis_free_space, line_cross_size, max_major_ascent,
        max_major_descent, max_minor_ascent, max_minor_descent,
        main_axis_auto_margin_count);

    // Ensure that we use the definite line cross-line if available.
    const LayoutUnit original_line_cross_size = line_cross_size;
    line_cross_size = definite_line_cross_size.value_or(line_cross_size);

    if (definite_line_cross_size.has_value() &&
        original_line_cross_size != line_cross_size) {
      ipid_depth_log.FPrint(
          "第 {} 行的交叉轴尺寸从根据内容计算的 {}px 覆盖为确定值 {}px。",
          line_index + 1, original_line_cross_size, line_cross_size);
    }

    ipid_depth_log.FPrint(
        "第 {} 行处理完成，创建 FlexLine 对象。最终参数：主轴剩余空间 {}px，"
        "交叉轴尺寸 {}px，主要基线上升高度 {}px，次要基线上升高度 {}px，"
        "auto margin 数量 {}。",
        line_index + 1, main_axis_free_space, line_cross_size, max_major_ascent,
        max_minor_ascent, main_axis_auto_margin_count);

    flex_lines->emplace_back(std::move(item_indices), main_axis_free_space,
                             line_cross_size, max_major_ascent,
                             max_minor_ascent, main_axis_auto_margin_count);

    sum_line_cross_size += line_cross_size;

    ipid_depth_log.FPrint(
        "第 {} 行添加完成，所有行的交叉轴尺寸总和更新为 {}px。", line_index + 1,
        sum_line_cross_size);
  }

  ipid_depth_log.FPrint("所有 {} 行处理完成，总的交叉轴尺寸为 {}px。",
                        flex_lines->size(), sum_line_cross_size);

  // Determine the intrinsic block-size if within the layout-pass.
  if (total_intrinsic_block_size_out) {
    ipid_depth_log.FPrint(
        "开始计算 flex "
        "容器的固有高度。这个值用于确定容器在垂直方向上的自然尺寸。");

    *total_intrinsic_block_size_out = ([&]() {
      LayoutUnit size = BorderScrollbarPadding().BlockSum();

      ipid_depth_log.FPrint(
          "固有高度的基础值为容器的 border + scrollbar + padding "
          "的垂直总和：{}px。",
          size);

      if (!flex_lines->empty()) {
        if (is_column_) {
          // Take the largest hypothetical main-size.
          size += result.max_sum_hypothetical_main_size;

          ipid_depth_log.FPrint(
              "当前是列 flex 容器，固有高度 = border+scrollbar+padding {}px + "
              "最大假定主轴尺寸 {}px = {}px。",
              BorderScrollbarPadding().BlockSum(),
              result.max_sum_hypothetical_main_size, size);
        } else {
          // Take the sum of all the line cross-sizes (and the gaps between
          // them).
          const LayoutUnit lines_gap =
              (flex_lines->size() - 1) * gap_between_lines_;
          size += sum_line_cross_size;
          size += lines_gap;

          ipid_depth_log.FPrint(
              "当前是行 flex 容器，固有高度 = border+scrollbar+padding {}px + "
              "所有行交叉轴尺寸总和 {}px + 行间间距 {}px = {}px。",
              BorderScrollbarPadding().BlockSum(), sum_line_cross_size,
              lines_gap, size);
        }
      } else if (Node().HasLineIfEmpty()) {
        const LayoutUnit empty_line_size =
            Node().EmptyLineBlockSize(GetBreakToken());
        size += empty_line_size;

        ipid_depth_log.FPrint(
            "容器没有 flex 行但有空行，固有高度 = border+scrollbar+padding "
            "{}px + "
            "空行高度 {}px = {}px。",
            BorderScrollbarPadding().BlockSum(), empty_line_size, size);
      } else {
        ipid_depth_log.FPrint(
            "容器没有任何内容，固有高度就是 border+scrollbar+padding：{}px。",
            size);
      }

      ipid_depth_log.FPrint(
          "调用 ClampIntrinsicBlockSize 来约束固有高度到合理范围，输入值为 "
          "{}px。",
          size);

      const LayoutUnit clamped_size =
          ClampIntrinsicBlockSize(GetConstraintSpace(), Node(), GetBreakToken(),
                                  BorderScrollbarPadding(), size);

      ipid_depth_log.FPrint("经过约束后的最终固有高度为 {}px。", clamped_size);

      return clamped_size;
    })();

    ipid_depth_log.FPrint("flex 容器的固有高度计算完成，最终值为 {}px。",
                          *total_intrinsic_block_size_out);
  }

  ipid_depth_log.FPrint(
      "PlaceFlexItems 函数执行完成。共处理了 {} 行 flex 项目，"
      "总交叉轴尺寸为 {}px{}。",
      flex_lines->size(), sum_line_cross_size,
      total_intrinsic_block_size_out
          ? ("，容器固有高度为 " +
             std::to_string(total_intrinsic_block_size_out->ToDouble()) + "px")
          : "");
}

void FlexLayoutAlgorithm::ApplyReversals(FlexLineVector* flex_lines) {
  if (is_wrap_reverse_) {
    flex_lines->Reverse();
  }

  if (is_reverse_direction_) {
    for (auto& flex_line : *flex_lines) {
      flex_line.item_indices.Reverse();
    }
  }
}

namespace {

LayoutUnit InitialContentPositionOffset(const StyleContentAlignmentData& data,
                                        LayoutUnit free_space,
                                        unsigned number_of_items,
                                        bool is_reverse) {
  switch (data.Distribution()) {
    case ContentDistributionType::kDefault:
      break;
    case ContentDistributionType::kSpaceBetween:
      if (free_space > LayoutUnit() && number_of_items > 1) {
        return LayoutUnit();
      }
      // Fallback to 'flex-start'.
      return is_reverse ? free_space : LayoutUnit();
    case ContentDistributionType::kSpaceAround:
      if (free_space > LayoutUnit() && number_of_items) {
        return free_space / (2 * number_of_items);
      }
      // Fallback to 'safe center'.
      return (free_space / 2).ClampNegativeToZero();
    case ContentDistributionType::kSpaceEvenly:
      if (free_space > LayoutUnit() && number_of_items) {
        return free_space / (number_of_items + 1);
      }
      // Fallback to 'safe center'.
      return (free_space / 2).ClampNegativeToZero();
    case ContentDistributionType::kStretch:
      // Fallback to 'flex-start'.
      return is_reverse ? free_space : LayoutUnit();
  }

  if (free_space <= LayoutUnit() &&
      data.Overflow() == OverflowAlignment::kSafe) {
    return LayoutUnit();
  }

  switch (data.GetPosition()) {
    case ContentPosition::kCenter:
      return free_space / 2;
    case ContentPosition::kStart:
      return LayoutUnit();
    case ContentPosition::kEnd:
      return free_space;
    case ContentPosition::kFlexEnd:
      return is_reverse ? LayoutUnit() : free_space;
    case ContentPosition::kFlexStart:
    case ContentPosition::kNormal:
    case ContentPosition::kBaseline:
    case ContentPosition::kLastBaseline:
      return is_reverse ? free_space : LayoutUnit();
    case ContentPosition::kLeft:
    case ContentPosition::kRight:
      NOTREACHED();
  }
}

LayoutUnit ContentDistributionSpace(const StyleContentAlignmentData& data,
                                    LayoutUnit free_space,
                                    unsigned number_of_items) {
  if (free_space <= LayoutUnit() || number_of_items <= 1) {
    return LayoutUnit();
  }
  switch (data.Distribution()) {
    case ContentDistributionType::kDefault:
    case ContentDistributionType::kStretch:
      return LayoutUnit();
    case ContentDistributionType::kSpaceBetween:
      return free_space / (number_of_items - 1);
    case ContentDistributionType::kSpaceEvenly:
      return free_space / (number_of_items + 1);
    case ContentDistributionType::kSpaceAround:
      return free_space / number_of_items;
  }
}

}  // namespace

LayoutResult::EStatus FlexLayoutAlgorithm::GiveItemsFinalPositionAndSize(
    FlexLineVector* flex_lines,
    Vector<EBreakBetween>* row_break_between_outputs) {
  IpidDepthLog ipid_depth_log(
      "flex_layout_algorithm.cc: GiveItemsFinalPositionAndSize");

  ipid_depth_log.FPrint(
      "开始执行 flex 布局算法的最后阶段：确定每个 flex 项目的最终位置和尺寸。\n"
      "当前 flex 容器方向：{}，是否多行：{}，是否反向：{}",
      ipid::btos(is_column_), ipid::btos(is_multi_line_),
      ipid::btos(is_reverse_direction_));

  DCHECK(!IsBreakInside(GetBreakToken()));

  const bool should_propagate_row_break_values =
      GetConstraintSpace().ShouldPropagateChildBreakValues();
  if (should_propagate_row_break_values) {
    ipid_depth_log.FPrint(
        "当前布局上下文要求传播子元素的分片断点信息（用于处理分页、分栏等情况）"
        "，"
        "因此需要为每个 flex 行创建断点值数组。");
    DCHECK(row_break_between_outputs);
    // The last row break between will store the final break-after to be
    // propagated to the container.
    if (!is_column_) {
      *row_break_between_outputs =
          Vector<EBreakBetween>(flex_lines->size() + 1, EBreakBetween::kAuto);
      ipid_depth_log.FPrint(
          "为行布局的 flexbox 创建了 {} 个断点值位置（行数 + 1），"
          "每个位置初始值为 kAuto。",
          flex_lines->size() + 1);
    } else {
      // For flex columns, we only need to store two values - one for
      // the break-before value of all combined columns, and the second for
      // for the break-after values for all combined columns.
      *row_break_between_outputs =
          Vector<EBreakBetween>(2, EBreakBetween::kAuto);
      ipid_depth_log.FPrint(
          "为列布局的 flexbox 创建了 2 个断点值位置：第一个存储所有列的 "
          "break-before 值，第二个存储所有列的 break-after 值。");
    }
  }

  // Nothing to do if we don't have any flex-lines.
  if (flex_lines->empty()) {
    ipid_depth_log.FPrint(
        "没有 flex 行需要处理（可能是因为没有 flex 项目或项目被过滤掉），"
        "直接返回布局成功状态。");
    return LayoutResult::kSuccess;
  }

  const auto& style = Style();
  const WritingDirectionMode writing_direction =
      GetConstraintSpace().GetWritingDirection();

  const StyleContentAlignmentData justify_content = ResolvedJustifyContent();
  const StyleContentAlignmentData align_content = style.AlignContent();

  ipid_depth_log.FPrint(
      "获取 flex 容器的对齐方式设置：\n"
      "justify-content: {}\n"
      "align-content: {}\n"
      "共有 {} 个 flex 行需要处理",
      ipid::GetStyleContentAlignmentDataString(justify_content),
      ipid::GetStyleContentAlignmentDataString(align_content),
      flex_lines->size());

  // Determine the cross-axis free-space.
  const wtf_size_t num_lines = flex_lines->size();
  const LayoutUnit cross_axis_content_size =
      (is_column_ ? (container_builder_.InlineSize() -
                     BorderScrollbarPadding().InlineSum())
                  : (total_block_size_ - BorderScrollbarPadding().BlockSum()))
          .ClampNegativeToZero();

  ipid_depth_log.FPrint(
      "计算交叉轴可用空间：\n"
      "容器 {} 尺寸：{}，减去 border+scrollbar+padding：{}，"
      "交叉轴内容区域尺寸：{}",
      is_column_ ? "宽度" : "高度",
      is_column_ ? container_builder_.InlineSize() : total_block_size_,
      is_column_ ? BorderScrollbarPadding().InlineSum()
                 : BorderScrollbarPadding().BlockSum(),
      cross_axis_content_size);

  LayoutUnit cross_axis_free_space = cross_axis_content_size;
  for (const FlexLine& line : *flex_lines) {
    cross_axis_free_space -= line.line_cross_size;
  }
  cross_axis_free_space -= (num_lines - 1) * gap_between_lines_;

  ipid_depth_log.FPrint(
      "交叉轴剩余空间计算：起始空间 {}，减去所有行的交叉轴尺寸后剩余 {}，"
      "再减去 {} 个行间间隙（每个 {}），最终剩余空间：{}",
      cross_axis_content_size,
      cross_axis_content_size -
          (cross_axis_free_space + (num_lines - 1) * gap_between_lines_),
      (num_lines - 1), gap_between_lines_, cross_axis_free_space);

  const bool is_align_content_stretch =
      align_content.Distribution() == ContentDistributionType::kStretch ||
      (align_content.GetPosition() == ContentPosition::kNormal &&
       align_content.Distribution() == ContentDistributionType::kDefault);

  if (!is_multi_line_) {
    // A single line flexbox will always be the cross-axis content-size.
    LayoutUnit original_size = flex_lines->back().line_cross_size;
    flex_lines->back().line_cross_size = cross_axis_content_size;
    cross_axis_free_space = LayoutUnit();
    ipid_depth_log.FPrint(
        "单行 flexbox 处理：将唯一的 flex 行的交叉轴尺寸从 {} 调整为整个"
        "交叉轴内容区域尺寸 {}，剩余空间重置为 0",
        original_size, cross_axis_content_size);
  } else if (cross_axis_free_space >= LayoutUnit() &&
             is_align_content_stretch) {
    // Stretch lines in a multi-line flexbox to the available free-space.
    const LayoutUnit delta = cross_axis_free_space / num_lines;
    ipid_depth_log.FPrint(
        "多行 flexbox 拉伸处理：align-content 设置为拉伸，剩余空间 {} > 0，"
        "将剩余空间平均分配给 {} 行，每行增加 {}",
        cross_axis_free_space, num_lines, delta);
    for (FlexLine& line : *flex_lines) {
      line.line_cross_size += delta;
    }
    cross_axis_free_space = LayoutUnit();
  } else if (cross_axis_free_space < LayoutUnit()) {
    ipid_depth_log.FPrint(
        "交叉轴剩余空间为负值 {}，表示内容超出容器，无法进行拉伸",
        cross_axis_free_space);
  } else if (!is_align_content_stretch) {
    ipid_depth_log.FPrint(
        "align-content 未设置为拉伸（当前：位置={}，分布={}），"
        "保持 {} 的剩余空间用于后续分布",
        ipid::GetContentPositionString(align_content.GetPosition()),
        ipid::GetContentDistributionTypeString(align_content.Distribution()),
        cross_axis_free_space);
  }

  const LayoutUnit space_between_lines =
      ContentDistributionSpace(align_content, cross_axis_free_space, num_lines);
  LayoutUnit line_cross_axis_offset =
      (is_column_ ? BorderScrollbarPadding().inline_start
                  : BorderScrollbarPadding().block_start) +
      InitialContentPositionOffset(align_content, cross_axis_free_space,
                                   num_lines, is_wrap_reverse_);

  ipid_depth_log.FPrint(
      "计算行间距离和起始位置：\n"
      "根据 align-content 设置，行间额外间距：{}，\n"
      "起始偏移：border+scrollbar+padding 的 {} 边距 {} + "
      "内容位置偏移 {}，总起始偏移：{}",
      space_between_lines, is_column_ ? "左" : "上",
      is_column_ ? BorderScrollbarPadding().inline_start
                 : BorderScrollbarPadding().block_start,
      line_cross_axis_offset - (is_column_
                                    ? BorderScrollbarPadding().inline_start
                                    : BorderScrollbarPadding().block_start),
      line_cross_axis_offset);

  BaselineAccumulator baseline_accumulator(style);
  LayoutResult::EStatus status = LayoutResult::kSuccess;

  std::optional<GapAccumulator> gap_accumulator = std::nullopt;
  if (RuntimeEnabledFeatures::CSSGapDecorationEnabled() &&
      Style().HasGapRule()) {
    gap_accumulator = GapAccumulator(gap_between_items_, gap_between_lines_,
                                     flex_lines->size(), flex_items_.size(),
                                     &container_builder_, is_column_);
    ipid_depth_log.FPrint(
        "启用了 CSS gap 装饰功能，创建间隙累加器来处理项目间间隙 {} 和行间间隙 "
        "{}",
        gap_between_items_, gap_between_lines_);
  }

  for (wtf_size_t flex_line_idx = 0; flex_line_idx < flex_lines->size();
       ++flex_line_idx) {
    if (layout_info_for_devtools_) [[unlikely]] {
      layout_info_for_devtools_->lines.push_back(DevtoolsFlexInfo::Line());
    }

    FlexLine& flex_line = (*flex_lines)[flex_line_idx];
    flex_line.cross_axis_offset = line_cross_axis_offset;

    ipid_depth_log.FPrint(
        "\n处理第 {} 个 flex 行（索引 {}）：\n"
        "交叉轴偏移设为：{}，行交叉轴尺寸：{}，包含 {} 个项目",
        flex_line_idx + 1, flex_line_idx, line_cross_axis_offset,
        flex_line.line_cross_size, flex_line.item_indices.size());

    bool is_first_line = flex_line_idx == 0;
    bool is_last_line = flex_line_idx == flex_lines->size() - 1;
    if (!InvolvedInBlockFragmentation(container_builder_) && !is_column_) {
      baseline_accumulator.AccumulateLine(flex_line, is_first_line,
                                          is_last_line);
    }

    const bool should_apply_main_axis_auto_margin =
        flex_line.main_axis_auto_margin_count &&
        flex_line.main_axis_free_space > LayoutUnit();

    const LayoutUnit main_axis_free_space =
        should_apply_main_axis_auto_margin ? LayoutUnit()
                                           : flex_line.main_axis_free_space;
    const LayoutUnit main_axis_auto_margin =
        should_apply_main_axis_auto_margin
            ? flex_line.main_axis_free_space /
                  flex_line.main_axis_auto_margin_count
            : LayoutUnit();

    ipid_depth_log.FPrint(
        "主轴自动 margin 处理：\n"
        "行内有 {} 个自动 margin，主轴剩余空间：{}，\n"
        "{}",
        flex_line.main_axis_auto_margin_count, flex_line.main_axis_free_space,
        should_apply_main_axis_auto_margin
            ? std::format("应用自动 margin：每个自动 margin 分配 "
                          "{}，用于布局的剩余空间为 0",
                          main_axis_auto_margin.ToString().Utf8())
            : std::format("不应用自动 margin（{}），用于内容分布的剩余空间：{}",
                          flex_line.main_axis_auto_margin_count > 0
                              ? "剩余空间 <= 0"
                              : "无自动 margin",
                          main_axis_free_space.ToString().Utf8()));

    const wtf_size_t line_items_size = flex_line.item_indices.size();
    const LayoutUnit space_between_items = ContentDistributionSpace(
        justify_content, main_axis_free_space, line_items_size);
    LayoutUnit main_axis_offset =
        (is_column_ ? BorderScrollbarPadding().block_start
                    : BorderScrollbarPadding().inline_start) +
        InitialContentPositionOffset(justify_content, main_axis_free_space,
                                     line_items_size, is_reverse_direction_);

    ipid_depth_log.FPrint(
        "主轴项目分布计算：\n"
        "项目间额外间距：{}，起始偏移：{} + {} = {}",
        space_between_items,
        is_column_ ? BorderScrollbarPadding().block_start
                   : BorderScrollbarPadding().inline_start,
        main_axis_offset - (is_column_ ? BorderScrollbarPadding().block_start
                                       : BorderScrollbarPadding().inline_start),
        main_axis_offset);

    wtf_size_t item_index_in_line = 0;

    for (wtf_size_t item_index : flex_line.item_indices) {
      const FlexItem& item = flex_items_[item_index];

      ipid_depth_log.FPrint("\n  处理行内第 {} 个项目（全局索引 {}）：{}",
                            item_index_in_line + 1, item_index,
                            ipid::GetNodeStr(item.block_node));

      const ConstraintSpace child_space = BuildSpaceForLayout(
          item.block_node, item.alignment,
          item.is_initial_block_size_indefinite,
          /* override_inline_size */ std::nullopt, item.FlexedBorderBoxSize(),
          flex_line.line_cross_size);
      const LayoutResult* layout_result = item.block_node.Layout(child_space);

      ipid_depth_log.FPrint(
          "  为项目创建布局约束空间并执行布局：\n"
          "  项目对齐方式：{}，初始高度是否不定：{}，\n"
          "  flex 后的边框盒尺寸：{}，所在行的交叉轴尺寸：{}，\n"
          "  布局结果状态：{}",
          ipid::GetItemPositionString(item.alignment),
          ipid::btos(item.is_initial_block_size_indefinite),
          item.FlexedBorderBoxSize(), flex_line.line_cross_size,
          ipid::GetLayoutResultStatusString(
              static_cast<int>(layout_result->Status())));

      const auto& item_style = item.block_node.Style();

      if (should_propagate_row_break_values) {
        auto item_break_before = JoinFragmentainerBreakValues(
            item_style.BreakBefore(), layout_result->InitialBreakBefore());
        auto item_break_after = JoinFragmentainerBreakValues(
            item_style.BreakAfter(), layout_result->FinalBreakAfter());

        // The break-before and break-after values of flex items in a flex row
        // are propagated to the row itself. Accumulate the BreakBetween values
        // for each row ahead of time so that they can be stored on the break
        // token for future use.
        //
        // https://drafts.csswg.org/css-flexbox-1/#pagination
        if (!is_column_) {
          (*row_break_between_outputs)[flex_line_idx] =
              JoinFragmentainerBreakValues(
                  (*row_break_between_outputs)[flex_line_idx],
                  item_break_before);
          (*row_break_between_outputs)[flex_line_idx + 1] =
              JoinFragmentainerBreakValues(
                  (*row_break_between_outputs)[flex_line_idx + 1],
                  item_break_after);
        } else {
          // Treat all columns as a "row" of columns, and accumulate the initial
          // and final break values for all columns, which will be propagated to
          // the container.
          if (item_index == flex_line.item_indices.front()) {
            (*row_break_between_outputs)[0] = JoinFragmentainerBreakValues(
                (*row_break_between_outputs)[0], item_break_before);
          }
          if (item_index == flex_line.item_indices.back()) {
            (*row_break_between_outputs)[1] = JoinFragmentainerBreakValues(
                (*row_break_between_outputs)[1], item_break_after);
          }
        }
      }

      const auto& physical_fragment =
          To<PhysicalBoxFragment>(layout_result->GetPhysicalFragment());
      const LogicalBoxFragment fragment(writing_direction, physical_fragment);
      const LayoutUnit cross_axis_size =
          is_column_ ? fragment.InlineSize() : fragment.BlockSize();

      ipid_depth_log.FPrint("  项目布局完成，交叉轴实际尺寸：{}",
                            cross_axis_size);

      PhysicalBoxStrut physical_margins = item.initial_margins;
      const PhysicalToFlex<LayoutUnit&> margin(
          writing_direction, is_column_, physical_margins.top,
          physical_margins.right, physical_margins.bottom,
          physical_margins.left);

      const LayoutUnit cross_axis_space = flex_line.line_cross_size -
                                          margin.CrossStart() -
                                          cross_axis_size - margin.CrossEnd();

      ipid_depth_log.FPrint(
          "  计算交叉轴可用空间：行尺寸 {} - 起始 margin {} - 项目尺寸 {} - "
          "结束 margin {} = {}",
          flex_line.line_cross_size, margin.CrossStart(), cross_axis_size,
          margin.CrossEnd(), cross_axis_space);

      // Apply any auto margins.
      {
        const PhysicalToFlex is_margin_auto(writing_direction, is_column_,
                                            item_style.MarginTop().IsAuto(),
                                            item_style.MarginRight().IsAuto(),
                                            item_style.MarginBottom().IsAuto(),
                                            item_style.MarginLeft().IsAuto());

        // Cross-axis margins are handled in the typical way.
        const LayoutUnit margin_space = cross_axis_space.ClampNegativeToZero();
        if (is_margin_auto.CrossStart() && is_margin_auto.CrossEnd()) {
          margin.CrossStart() = margin_space / 2;
          margin.CrossEnd() = margin_space / 2;
          ipid_depth_log.FPrint(
              "  交叉轴自动 margin：起始和结束都是 auto，各分配 {}",
              margin_space / 2);
        } else if (is_margin_auto.CrossStart()) {
          margin.CrossStart() = margin_space;
          ipid_depth_log.FPrint(
              "  交叉轴自动 margin：仅起始是 auto，分配全部可用空间 {}",
              margin_space);
        } else if (is_margin_auto.CrossEnd()) {
          margin.CrossEnd() = margin_space;
          ipid_depth_log.FPrint(
              "  交叉轴自动 margin：仅结束是 auto，分配全部可用空间 {}",
              margin_space);
        }

        // Main-axis margins are distributed to evenly across the whole line.
        if (is_margin_auto.MainStart()) {
          margin.MainStart() = main_axis_auto_margin;
          ipid_depth_log.FPrint("  主轴起始 margin 设为 auto，分配 {}",
                                main_axis_auto_margin);
        }
        if (is_margin_auto.MainEnd()) {
          margin.MainEnd() = main_axis_auto_margin;
          ipid_depth_log.FPrint("  主轴结束 margin 设为 auto，分配 {}",
                                main_axis_auto_margin);
        }
      }

      // Determine the cross-axis offset based on the item alignment.
      const LayoutUnit cross_axis_offset = ([&]() {
        const bool is_safe =
            !is_webkit_box_ &&
            item_style
                    .ResolvedAlignSelf(
                        {ItemPosition::kStretch, OverflowAlignment::kDefault},
                        &Style())
                    .Overflow() == OverflowAlignment::kSafe;
        const LayoutUnit space =
            is_safe ? cross_axis_space.ClampNegativeToZero() : cross_axis_space;

        ipid_depth_log.FPrint(
            "  计算交叉轴偏移：对齐方式 {}，安全对齐：{}，可用空间：{}",
            ipid::GetItemPositionString(item.alignment), ipid::btos(is_safe),
            space);

        LayoutUnit offset;
        switch (item.alignment) {
          case ItemPosition::kCenter:
            offset = space / 2;
            ipid_depth_log.FPrint("    居中对齐，偏移 {}", offset);
            break;
          case ItemPosition::kFlexStart:
            ipid_depth_log.FPrint("    flex-start 对齐，偏移 0");
            break;
          case ItemPosition::kFlexEnd:
            offset = space;
            ipid_depth_log.FPrint("    flex-end 对齐，偏移 {}", offset);
            break;
          case ItemPosition::kStretch:
            offset = is_wrap_reverse_ ? space : LayoutUnit();
            ipid_depth_log.FPrint("    拉伸对齐，{}偏移 {}",
                                  is_wrap_reverse_ ? "反向包裹，" : "", offset);
            break;
          case ItemPosition::kBaseline:
          case ItemPosition::kLastBaseline: {
            const bool is_major = item.baseline_group == BaselineGroup::kMajor;
            const LayoutUnit ascent = BaselineAscent(item, physical_fragment);
            const LayoutUnit max_ascent =
                is_major ? flex_line.major_baseline : flex_line.minor_baseline;
            const LayoutUnit baseline_delta = max_ascent - ascent;
            offset = is_major ? baseline_delta : space - baseline_delta;
            ipid_depth_log.FPrint(
                "    "
                "基线对齐（{}基线组），项目基线上升距：{}，行最大上升距：{}，"
                "基线差值：{}，最终偏移：{}",
                is_major ? "主要" : "次要", ascent, max_ascent, baseline_delta,
                offset);
            break;
          }
          default:
            NOTREACHED() << "All other values shouldn't be possible.";
        }

        LayoutUnit final_offset =
            line_cross_axis_offset + offset + margin.CrossStart();
        ipid_depth_log.FPrint(
            "  最终交叉轴位置：行偏移 {} + 对齐偏移 {} + 起始 margin {} = {}",
            line_cross_axis_offset, offset, margin.CrossStart(), final_offset);
        return final_offset;
      })();

      main_axis_offset += margin.MainStart();

      const LogicalOffset offset =
          is_column_ ? LogicalOffset(cross_axis_offset, main_axis_offset)
                     : LogicalOffset(main_axis_offset, cross_axis_offset);

      ipid_depth_log.FPrint(
          "  确定项目最终位置：主轴偏移更新为 {}（加上起始 margin {}），"
          "逻辑偏移 ({}, {})",
          main_axis_offset, margin.MainStart(), offset.inline_offset,
          offset.block_offset);

      main_axis_offset += item.FlexedBorderBoxSize() + margin.MainEnd() +
                          space_between_items + gap_between_items_;

      ipid_depth_log.FPrint(
          "  更新下一项目的主轴起始位置：当前 {} + 项目尺寸 {} + 结束 margin "
          "{} + "
          "项目间距 {} + 间隙 {} = {}",
          main_axis_offset - item.FlexedBorderBoxSize() - margin.MainEnd() -
              space_between_items - gap_between_items_,
          item.FlexedBorderBoxSize(), margin.MainEnd(), space_between_items,
          gap_between_items_, main_axis_offset);

      const BoxStrut logical_margins =
          physical_margins.ConvertToLogical(writing_direction);

      if (!InvolvedInBlockFragmentation(container_builder_)) {
        container_builder_.AddResult(*layout_result, offset, logical_margins);
        baseline_accumulator.AccumulateItem(fragment, offset.block_offset,
                                            is_first_line, is_last_line);
        ipid_depth_log.FPrint("  将项目添加到容器：位置 ({}, {})，margin {}",
                              offset.inline_offset, offset.block_offset,
                              ipid::GetBoxStrutString(logical_margins));
      } else {
        // Store the information we need for later if we have fragmentation.
        flex_line.line_items_data.emplace_back(
            item.block_node, item.item_index, offset, item.alignment,
            item.FlexedBorderBoxSize(), logical_margins.block_end,
            fragment.BlockSize(), item.is_initial_block_size_indefinite,
            item.is_used_flex_basis_indefinite,
            layout_result->HasDescendantThatDependsOnPercentageBlockSize());
        ipid_depth_log.FPrint("  存储项目信息用于分片处理（不立即添加到容器）");
      }

      if (PropagateFlexItemInfo(item, physical_fragment, physical_margins,
                                flex_line_idx, offset) ==
          LayoutResult::kNeedsRelayoutWithNoChildScrollbarChanges) {
        status = LayoutResult::kNeedsRelayoutWithNoChildScrollbarChanges;
        ipid_depth_log.FPrint(
            "  项目信息传播返回需要重新布局状态（滚动条变化）");
      }

      if (gap_accumulator) {
        gap_accumulator->BuildGapIntersectionPointsForCurrentItem(
            *flex_lines, flex_line_idx, item_index_in_line, offset);
      }

      item_index_in_line++;
    }

    line_cross_axis_offset +=
        flex_line.line_cross_size + space_between_lines + gap_between_lines_;

    ipid_depth_log.FPrint(
        "第 {} 行处理完成，更新下一行的交叉轴偏移：当前 {} + 行尺寸 {} + "
        "行间距 {} + "
        "行间隙 {} = {}",
        flex_line_idx + 1,
        line_cross_axis_offset - flex_line.line_cross_size -
            space_between_lines - gap_between_lines_,
        flex_line.line_cross_size, space_between_lines, gap_between_lines_,
        line_cross_axis_offset);

    if (gap_accumulator) {
      gap_accumulator->FinishedProcessingLine(flex_line_idx);
    }
  }

  if (auto first_baseline = baseline_accumulator.FirstBaseline())
    container_builder_.SetFirstBaseline(*first_baseline);
  if (auto last_baseline = baseline_accumulator.LastBaseline())
    container_builder_.SetLastBaseline(*last_baseline);

  ipid_depth_log.FPrint(
      "设置容器基线：第一基线 {}，最后基线 {}",
      baseline_accumulator.FirstBaseline().has_value()
          ? std::to_string(baseline_accumulator.FirstBaseline()->ToInt())
          : "无",
      baseline_accumulator.LastBaseline().has_value()
          ? std::to_string(baseline_accumulator.LastBaseline()->ToInt())
          : "无");

  // TODO(crbug.com/1131352): Avoid control-specific handling.
  if (Node().IsSlider()) {
    DCHECK(!InvolvedInBlockFragmentation(container_builder_));
    container_builder_.ClearBaselines();
    ipid_depth_log.FPrint("特殊处理：清除滑块控件的基线");
  }

  if (gap_accumulator) {
    container_builder_.SetGapGeometry(gap_accumulator->BuildGapGeometry());
    ipid_depth_log.FPrint("设置间隙几何信息到容器");
  }

  ipid_depth_log.FPrint(
      "flex 布局最终阶段完成，返回状态：{}",
      ipid::GetLayoutResultStatusString(static_cast<int>(status)));

  // Signal if we need to relayout with new child scrollbar information.
  return status;
}

LayoutResult::EStatus
FlexLayoutAlgorithm::GiveItemsFinalPositionAndSizeForFragmentation(
    FlexLineVector* flex_lines,
    Vector<EBreakBetween>* row_break_between_outputs,
    FlexBreakTokenData::FlexBreakBeforeRow* break_before_row,
    LayoutUnit* total_intrinsic_block_size) {
  IpidDepthLog ipid_depth_log(
      "flex_layout_algorithm.cc: "
      "GiveItemsFinalPositionAndSizeForFragmentation");

  DCHECK(InvolvedInBlockFragmentation(container_builder_));
  DCHECK(flex_lines);
  DCHECK(row_break_between_outputs);
  DCHECK(break_before_row);

  ipid_depth_log.FPrint(
      "开始为 {} 容器中的 flex 项目进行分片处理，确定最终位置和尺寸。\n"
      "Flex 行数量：{}，当前 break_before_row 状态：{}，\n"
      "total_intrinsic_block_size 初始值：{}",
      is_column_ ? "column" : "row", flex_lines->size(),
      ipid::GetFlexBreakBeforeRowString(static_cast<int>(*break_before_row)),
      *total_intrinsic_block_size);

  FlexItemIterator item_iterator(*flex_lines, GetBreakToken(), is_column_);

  Vector<bool> has_inflow_child_break_inside_line(flex_lines->size(), false);
  bool needs_earlier_break_in_column = false;
  LayoutResult::EStatus status = LayoutResult::kSuccess;
  LayoutUnit fragmentainer_space = FragmentainerSpaceLeftForChildren();

  ipid_depth_log.FPrint(
      "初始化分片状态变量：\n"
      "- 每行是否有子元素内部分片的标记数组长度：{}\n"
      "- 当前分片容器剩余可用高度：{}",
      has_inflow_child_break_inside_line.size(), fragmentainer_space);

  HeapVector<FlexColumnBreakInfo> column_break_info;
  if (is_column_) {
    column_break_info = HeapVector<FlexColumnBreakInfo>(flex_lines->size());
    ipid_depth_log.FPrint(
        "[Column 分片] 当前为 column flex 容器，为每一列（共 {} "
        "列）初始化分片信息数组。",
        flex_lines->size());
  }

  LayoutUnit previously_consumed_block_size;
  LayoutUnit offset_in_stitched_container;
  if (IsBreakInside(GetBreakToken())) {
    previously_consumed_block_size = GetBreakToken()->ConsumedBlockSize();
    offset_in_stitched_container = previously_consumed_block_size;

    ipid_depth_log.FPrint(
        "[分片恢复] 检测到当前 flex 容器的分片令牌指示已经有内容被分片，\n"
        "之前在其他分片容器中已消费的高度：{}，\n"
        "在拼接容器（忽略分片情况的虚拟容器）中的偏移：{}",
        previously_consumed_block_size, offset_in_stitched_container);

    if (Style().BoxDecorationBreak() == EBoxDecorationBreak::kClone &&
        offset_in_stitched_container != LayoutUnit::Max()) {
      // We want to deal with item offsets that we would have had had we not
      // been fragmented, and then add unused space caused by fragmentation, and
      // then calculate a block-offset relatively to the current fragment. In
      // the slicing box decoration model, that's simply about adding and
      // subtracting previously consumed block-size.
      //
      // For the cloning box decoration model, we need to subtract space used by
      // all cloned box decorations that wouldn't have been there in the slicing
      // model. That is: all box decorations from previous fragments, except the
      // initial block-start decoration of the first fragment.
      int preceding_fragment_count = GetBreakToken()->SequenceNumber() + 1;
      LayoutUnit decoration_adjustment =
          preceding_fragment_count * BorderScrollbarPadding().BlockSum() -
          BorderScrollbarPadding().block_start;

      ipid_depth_log.FPrint(
          "[盒装饰克隆模式] 当前容器的 box-decoration-break 为 clone "
          "模式，需要计算克隆装饰的空间占用。\n"
          "前序分片数量：{}，每个分片的边框+滚动条+内边距高度：{}，\n"
          "需要减去的装饰空间：{}，调整前的拼接容器偏移：{}，调整后：{}",
          preceding_fragment_count, BorderScrollbarPadding().BlockSum(),
          decoration_adjustment, offset_in_stitched_container,
          offset_in_stitched_container - decoration_adjustment);

      offset_in_stitched_container -= decoration_adjustment;
    }
  }

  BaselineAccumulator baseline_accumulator(Style());
  bool broke_before_row =
      *break_before_row != FlexBreakTokenData::kNotBreakBeforeRow;

  ipid_depth_log.FPrint(
      "开始主循环处理每个 flex 项目。\n"
      "baseline_accumulator 已初始化，broke_before_row 状态：{}",
      ipid::btos(broke_before_row));

  for (auto entry = item_iterator.NextItem(broke_before_row);
       FlexItemData* flex_item = entry.flex_item;
       entry = item_iterator.NextItem(broke_before_row)) {
    wtf_size_t flex_item_idx = entry.flex_item_idx;
    wtf_size_t flex_line_idx = entry.flex_line_idx;
    FlexLine& flex_line = (*flex_lines)[flex_line_idx];
    const auto* item_break_token = To<BlockBreakToken>(entry.token);
    bool last_item_in_line =
        flex_item_idx == flex_line.line_items_data.size() - 1;

    bool is_first_line = flex_line_idx == 0;
    bool is_last_line = flex_line_idx == flex_lines->size() - 1;

    ipid_depth_log.FPrint(
        "== 处理 flex 项目 {} ==\n"
        "项目在行内索引：{}，行索引：{}，是否为行内最后一个项目：{}，\n"
        "是否为第一行：{}，是否为最后一行：{}，\n"
        "项目信息：{}，\n"
        "所属行信息：{}",
        ipid::GetNodeStr(flex_item->block_node), flex_item_idx, flex_line_idx,
        ipid::btos(last_item_in_line), ipid::btos(is_first_line),
        ipid::btos(is_last_line), ipid::GetFlexItemDataString(*flex_item),
        ipid::GetFlexLineString(flex_line));

    // A child break in a parallel flow doesn't affect whether we should
    // break here or not. But if the break happened in the same flow, we'll now
    // just finish layout of the fragment. No more siblings should be processed.
    if (!is_column_) {
      if (flex_line_idx != 0 &&
          has_inflow_child_break_inside_line[flex_line_idx - 1]) {
        ipid_depth_log.FPrint(
            "[Row 分片中断] 当前为 row flex 容器，且前一行（索引 "
            "{}）已有子元素在内部分片，\n"
            "根据分片规则，停止处理后续项目。",
            flex_line_idx - 1);
        break;
      }
    } else {
      // If we are relaying out as a result of an early break, and we have early
      // breaks for more than one column, they will be stored in
      // |additional_early_breaks_|. Keep |early_break_| consistent with that of
      // the current column.
      if (additional_early_breaks_ &&
          flex_line_idx < additional_early_breaks_->size()) {
        early_break_ = (*additional_early_breaks_)[flex_line_idx];
        ipid_depth_log.FPrint(
            "[Column 早期分片] 当前为 column flex 容器，从 "
            "additional_early_breaks_ 中恢复列 {} 的早期分片信息。",
            flex_line_idx);
      } else if (early_break_ && flex_line_idx != 0) {
        early_break_ = nullptr;
        ipid_depth_log.FPrint(
            "[Column 早期分片] 当前列索引为 {}，清除之前的早期分片信息。",
            flex_line_idx);
      }

      if (has_inflow_child_break_inside_line[flex_line_idx]) {
        ipid_depth_log.FPrint(
            "[Column 分片中断] 当前列（索引 "
            "{}）已有子元素在内部分片，跳转到下一列。",
            flex_line_idx);
        if (!last_item_in_line)
          item_iterator.NextLine();
        continue;
      }
    }

    LayoutUnit row_block_offset =
        !is_column_ ? flex_line.cross_axis_offset : LayoutUnit();
    const LogicalOffset original_offset = flex_item->offset;
    LogicalOffset offset = original_offset;

    ipid_depth_log.FPrint(
        "计算项目在当前分片中的位置偏移：\n"
        "row_block_offset（行的高度偏移）：{}，\n"
        "原始偏移（拼接容器中的逻辑偏移）：({}, {})，\n"
        "当前工作偏移：({}, {})",
        row_block_offset, original_offset.inline_offset,
        original_offset.block_offset, offset.inline_offset,
        offset.block_offset);

    // If a row or item broke before, subsequent items and lines need to be
    // adjusted by the expansion amount.
    LayoutUnit individual_item_adjustment;
    if (item_break_token && item_break_token->IsBreakBefore()) {
      ipid_depth_log.FPrint(
          "[项目前分片] 检测到项目有 break-before 分片令牌，类型：{}",
          item_break_token->IsForcedBreak() ? "强制分片" : "非强制分片");

      if (item_break_token->IsForcedBreak()) {
        // We had previously updated the adjustment to subtract out the total
        // consumed block size up to the break. Now add the total consumed
        // block size in the previous fragmentainer to get the total amount
        // the item or row expanded by. This allows for things like margins
        // and alignment offsets to not get sliced by a forced break.
        flex_line.item_offset_adjustment += offset_in_stitched_container;
        ipid_depth_log.FPrint(
            "[强制分片调整] 这是一个强制分片，将拼接容器偏移 {} "
            "加到行的项目偏移调整中。\n"
            "调整后的 flex_line.item_offset_adjustment：{}",
            offset_in_stitched_container, flex_line.item_offset_adjustment);
      } else if (!is_column_ && flex_item_idx == 0 && broke_before_row) {
        // If this is the first time we are handling a break before a row,
        // adjust the offset of items in the row to accommodate the break. The
        // following cases need to be considered:
        //
        // 1. If we are not the first line in the container, and the previous
        // sibling row overflowed the fragmentainer in the block axis, flex
        // items in the current row should be adjusted upward in the block
        // direction to account for the overflowed content.
        //
        // 2. Otherwise, the current row gap should be decreased by the amount
        // of extra space in the previous fragmentainer remaining after the
        // block-end of the previous row. The reason being that we should not
        // clamp row gaps between breaks, similarly to how flex item margins are
        // handled during fragmentation.
        //
        // 3. If the entire row gap was accounted for in the previous
        // fragmentainer, the block-offsets of the flex items in the current row
        // will need to be adjusted downward in the block direction to
        // accommodate the extra space consumed by the container.
        ipid_depth_log.FPrint(
            "[行前分片调整] 当前为 row flex 容器的第一个项目，且行有 "
            "break-before，开始计算行偏移调整。");

        if (*break_before_row == FlexBreakTokenData::kAtStartOfBreakBeforeRow) {
          // Calculate the amount of space remaining in the previous
          // fragmentainer after the block-end of the previous flex row, if any.
          LayoutUnit previous_row_end =
              is_first_line ? LayoutUnit()
                            : (*flex_lines)[flex_line_idx - 1].LineCrossEnd();
          LayoutUnit previous_fragmentainer_unused_space =
              (offset_in_stitched_container - previous_row_end)
                  .ClampNegativeToZero();

          ipid_depth_log.FPrint(
              "[行间隙计算] 前一行的结束位置：{}，\n"
              "拼接容器偏移：{}，前一分片容器剩余空间：{}",
              previous_row_end, offset_in_stitched_container,
              previous_fragmentainer_unused_space);

          // If there was any remaining space after the previous flex line,
          // determine how much of the row gap was consumed in the previous
          // fragmentainer, if any.
          LayoutUnit consumed_row_gap;
          if (previous_fragmentainer_unused_space) {
            LayoutUnit total_row_block_offset =
                row_block_offset + flex_line.item_offset_adjustment;
            LayoutUnit row_gap = total_row_block_offset - previous_row_end;
            DCHECK_GE(row_gap, LayoutUnit());
            consumed_row_gap =
                std::min(row_gap, previous_fragmentainer_unused_space);

            ipid_depth_log.FPrint(
                "[行间隙消费] 计算在前一分片容器中消费的行间隙：\n"
                "总行高度偏移：{}，行间隙：{}，消费的行间隙：{}",
                total_row_block_offset, row_gap, consumed_row_gap);
          }

          // Adjust the item offsets to account for any overflow or consumed row
          // gap in the previous fragmentainer.
          LayoutUnit row_adjustment = offset_in_stitched_container -
                                      previous_row_end - consumed_row_gap;
          flex_line.item_offset_adjustment += row_adjustment;

          ipid_depth_log.FPrint(
              "[行偏移调整] 计算得到的行调整量：{}，\n"
              "调整后的 flex_line.item_offset_adjustment：{}",
              row_adjustment, flex_line.item_offset_adjustment);
        }
      } else {
        LayoutUnit total_item_block_offset =
            offset.block_offset + flex_line.item_offset_adjustment;
        individual_item_adjustment =
            (offset_in_stitched_container - total_item_block_offset)
                .ClampNegativeToZero();
        // For items in a row, the offset adjustment due to a break before
        // should only apply to the item itself and not to the entire row.
        if (is_column_) {
          flex_line.item_offset_adjustment += individual_item_adjustment;
        }
      }
    }

    if (IsBreakInside(item_break_token)) {
      offset.block_offset = BorderScrollbarPadding().block_start;
    } else if (IsBreakInside(GetBreakToken())) {
      // Convert block offsets from stitched coordinate system offsets to being
      // relative to the current fragment. Include space taken up by any cloned
      // block-start decorations (i.e. exclude it from the adjustment).
      LayoutUnit offset_adjustment = offset_in_stitched_container -
                                     flex_line.item_offset_adjustment -
                                     BorderScrollbarPadding().block_start;

      offset.block_offset -= offset_adjustment;
      if (!is_column_) {
        offset.block_offset += individual_item_adjustment;
        row_block_offset -= offset_adjustment;
      }
    }

    const EarlyBreak* early_break_in_child = nullptr;
    if (early_break_) [[unlikely]] {
      if (!is_column_)
        container_builder_.SetLineCount(flex_line_idx);
      if (IsEarlyBreakTarget(*early_break_, container_builder_,
                             flex_item->block_node)) {
        container_builder_.AddBreakBeforeChild(flex_item->block_node,
                                               kBreakAppealPerfect,
                                               /* is_forced_break */ false);
        if (early_break_->Type() == EarlyBreak::kLine) {
          *break_before_row = FlexBreakTokenData::kAtStartOfBreakBeforeRow;
        }
        ConsumeRemainingFragmentainerSpace(offset_in_stitched_container,
                                           &flex_line);
        // For column flex containers, continue to the next column. For rows,
        // continue until we've processed all items in the current row.
        has_inflow_child_break_inside_line[flex_line_idx] = true;
        if (is_column_) {
          if (!last_item_in_line)
            item_iterator.NextLine();
        } else if (last_item_in_line) {
          DCHECK_EQ(status, LayoutResult::kSuccess);
          break;
        }
        last_line_idx_to_process_first_child_ = flex_line_idx;
        continue;
      } else {
        early_break_in_child =
            EnterEarlyBreakInChild(flex_item->block_node, *early_break_);
      }
    }

    // If we are re-laying out one or more rows with an updated cross-size,
    // adjust the row info to reflect this change (but only if this is the first
    // time we are processing the current row in this layout pass).
    if (cross_size_adjustments_) [[unlikely]] {
      DCHECK(!is_column_);
      // Maps don't allow keys of 0, so adjust the index by 1.
      if (cross_size_adjustments_->Contains(flex_line_idx + 1) &&
          (last_line_idx_to_process_first_child_ == kNotFound ||
           last_line_idx_to_process_first_child_ < flex_line_idx)) {
        LayoutUnit row_block_size_adjustment =
            cross_size_adjustments_->find(flex_line_idx + 1)->value;
        flex_line.line_cross_size += row_block_size_adjustment;

        // Adjust any subsequent row offsets to reflect the current row's new
        // size.
        AdjustOffsetForNextLine(flex_lines, flex_line_idx,
                                row_block_size_adjustment);
      }
    }

    LayoutUnit line_cross_size = flex_line.line_cross_size;

    // If an item broke, its offset may have expanded (as the result of a
    // current or previous break before), in which case, we shouldn't expand by
    // the total line cross size. Otherwise, we would continue to expand the row
    // past the block-size of its items.
    if (!is_column_ && item_break_token) {
      line_cross_size -=
          offset_in_stitched_container -
          (original_offset.block_offset + flex_line.item_offset_adjustment) -
          item_break_token->ConsumedBlockSize();
    }

    const bool min_block_size_should_encompass_intrinsic_size =
        MinBlockSizeShouldEncompassIntrinsicSize(*flex_item);
    const ConstraintSpace child_space = BuildSpaceForLayout(
        flex_item->block_node, flex_item->alignment,
        flex_item->is_initial_block_size_indefinite,
        /* override_inline_size */ std::nullopt,
        flex_item->main_axis_final_size, line_cross_size, offset.block_offset,
        min_block_size_should_encompass_intrinsic_size);

    ipid_depth_log.FPrint(
        "[构建子元素布局空间] 为 flex 项目 {} 构建约束空间用于分片布局：\n"
        "- min_block_size_should_encompass_intrinsic_size: {}\n"
        "- 项目对齐方式：{}\n"
        "- 主轴最终尺寸：{}，行横轴尺寸：{}，高度偏移：{}\n"
        "- 构建的约束空间：{}",
        ipid::GetNodeStr(flex_item->block_node),
        ipid::btos(min_block_size_should_encompass_intrinsic_size),
        ipid::GetItemPositionString(flex_item->alignment),
        flex_item->main_axis_final_size, line_cross_size, offset.block_offset,
        ipid::GetConstraintSpaceString(child_space));

    const LayoutResult* layout_result = flex_item->block_node.Layout(
        child_space, item_break_token, early_break_in_child);

    ipid_depth_log.FPrint("[子元素布局完成] flex 项目 {} 的布局结果：{}",
                          ipid::GetNodeStr(flex_item->block_node),
                          ipid::GetLayoutResultString(layout_result));

    BreakStatus break_status = BreakStatus::kContinue;
    FlexColumnBreakInfo* current_column_break_info = nullptr;
    if (!early_break_ && GetConstraintSpace().HasBlockFragmentation()) {
      bool has_container_separation = false;
      if (!is_column_) {
        has_container_separation =
            offset.block_offset > row_block_offset &&
            (!item_break_token || (broke_before_row && flex_item_idx == 0 &&
                                   item_break_token->IsBreakBefore()));
        // Don't attempt to break before a row if the fist item is resuming
        // layout. In which case, the row should be resuming layout, as well.
        if (flex_item_idx == 0 &&
            (!item_break_token ||
             (item_break_token->IsBreakBefore() && broke_before_row))) {
          // Rows have no layout result, so if the row breaks before, we
          // will break before the first item in the row instead.
          bool row_container_separation = has_processed_first_line_;
          bool is_first_for_row = !item_break_token || broke_before_row;
          BreakStatus row_break_status = BreakBeforeRowIfNeeded(
              flex_line, row_block_offset,
              (*row_break_between_outputs)[flex_line_idx], flex_line_idx,
              flex_item->block_node, row_container_separation,
              is_first_for_row);
          if (row_break_status == BreakStatus::kBrokeBefore) {
            // If a gap overlaps a break, or is the last content before a break,
            // suppress it.
            if (flex_line_idx > 0) {
              // The available space should be dependent on previous row's block
              // end relative to this fragmentainer. This allows us to determine
              // the actual available space and how much of the gap is actually
              // consumed in this fragmentainer.
              LayoutUnit prev_flex_line_end =
                  (*flex_lines)[flex_line_idx - 1].LineCrossEnd() -
                  offset_in_stitched_container;
              UpdateOffsetAdjustmentForSuppressedRowGap(
                  gap_between_lines_,
                  /*previous_content_block_end=*/prev_flex_line_end,
                  &flex_line);
            }

            ConsumeRemainingFragmentainerSpace(offset_in_stitched_container,
                                               &flex_line);
            if (broke_before_row) {
              *break_before_row =
                  FlexBreakTokenData::kPastStartOfBreakBeforeRow;
            } else {
              *break_before_row = FlexBreakTokenData::kAtStartOfBreakBeforeRow;
            }
            DCHECK_EQ(status, LayoutResult::kSuccess);
            break;
          }
          *break_before_row = FlexBreakTokenData::kNotBreakBeforeRow;
          if (row_break_status == BreakStatus::kNeedsEarlierBreak) {
            status = LayoutResult::kNeedsEarlierBreak;
            break;
          }
          DCHECK_EQ(row_break_status, BreakStatus::kContinue);
        }
      } else {
        has_container_separation =
            !item_break_token &&
            ((last_line_idx_to_process_first_child_ != kNotFound &&
              last_line_idx_to_process_first_child_ >= flex_line_idx) ||
             offset.block_offset > LayoutUnit());

        // We may switch back and forth between columns, so we need to make sure
        // to use the break-after for the current column.
        if (flex_lines->size() > 1) {
          current_column_break_info = &column_break_info[flex_line_idx];
          container_builder_.SetPreviousBreakAfter(
              current_column_break_info->break_after);
        }
      }
      LayoutUnit fragmentainer_block_offset =
          FragmentainerOffsetForChildren() + offset.block_offset;

      ipid_depth_log.FPrint(
          "[分片决策] 调用 BreakBeforeChildIfNeeded "
          "判断是否需要在子元素前分片：\n"
          "- 子元素：{}，has_container_separation：{}，\n"
          "- 分片容器中的高度偏移：{}，is_row_item：{}，\n"
          "- current_column_break_info：{}",
          ipid::GetNodeStr(flex_item->block_node),
          ipid::btos(has_container_separation), fragmentainer_block_offset,
          ipid::btos(!is_column_),
          ipid::GetFlexColumnBreakInfoString(current_column_break_info));

      break_status = BreakBeforeChildIfNeeded(
          flex_item->block_node, *layout_result, fragmentainer_block_offset,
          has_container_separation, !is_column_, current_column_break_info);

      ipid_depth_log.FPrint(
          "[分片决策结果] BreakBeforeChildIfNeeded 返回状态：{}",
          ipid::GetBreakStatusString(static_cast<int>(break_status)));

      if (current_column_break_info) {
        current_column_break_info->break_after =
            container_builder_.PreviousBreakAfter();
      }
    }

    if (break_status == BreakStatus::kNeedsEarlierBreak) {
      if (current_column_break_info) {
        DCHECK(is_column_);
        DCHECK(current_column_break_info->early_break);
        if (!needs_earlier_break_in_column) {
          needs_earlier_break_in_column = true;
          container_builder_.SetEarlyBreak(
              current_column_break_info->early_break);
        }
        // Keep track of the early breaks for each column.
        AddColumnEarlyBreak(current_column_break_info->early_break,
                            flex_line_idx);
        if (!last_item_in_line)
          item_iterator.NextLine();
        continue;
      }
      status = LayoutResult::kNeedsEarlierBreak;
      break;
    }

    if (break_status == BreakStatus::kBrokeBefore) {
      // For column flex containers, suppress the row gap (i.e.
      // `gap_between_items_`) that may be split across fragmentainer breaks.
      if (is_column_ && flex_item_idx > 0) {
        UpdateOffsetAdjustmentForSuppressedRowGap(
            gap_between_items_,
            /*previous_content_block_end=*/intrinsic_block_size_, &flex_line);
      }
      ConsumeRemainingFragmentainerSpace(offset_in_stitched_container,
                                         &flex_line, current_column_break_info);
      // For column flex containers, continue to the next column. For rows,
      // continue until we've processed all items in the current row.
      has_inflow_child_break_inside_line[flex_line_idx] = true;
      if (is_column_) {
        if (!last_item_in_line)
          item_iterator.NextLine();
      } else if (last_item_in_line) {
        DCHECK_EQ(status, LayoutResult::kSuccess);
        break;
      }
      last_line_idx_to_process_first_child_ = flex_line_idx;
      continue;
    }

    const auto& physical_fragment =
        To<PhysicalBoxFragment>(layout_result->GetPhysicalFragment());

    LogicalBoxFragment fragment(GetConstraintSpace().GetWritingDirection(),
                                physical_fragment);

    bool is_at_block_end = !physical_fragment.GetBreakToken() ||
                           physical_fragment.GetBreakToken()->IsAtBlockEnd();
    LayoutUnit item_block_end = offset.block_offset + fragment.BlockSize();
    if (is_at_block_end) {
      // Only add the block-end margin if the item has reached the end of its
      // content. Then re-set it to avoid adding it more than once.
      item_block_end += flex_item->margin_block_end;
      flex_item->margin_block_end = LayoutUnit();
    } else {
      has_inflow_child_break_inside_line[flex_line_idx] = true;
    }

    // This item may have expanded due to fragmentation. Record how large the
    // shift was (if any). Only do this if the item has completed layout.
    if (is_column_) {
      LayoutUnit cloned_block_decorations;
      if (!is_at_block_end &&
          flex_item->block_node.Style().BoxDecorationBreak() ==
              EBoxDecorationBreak::kClone) {
        cloned_block_decorations = fragment.BoxDecorations().BlockSum();
      }

      // Cloned box decorations grow the border-box size of the flex item. In
      // flex layout, the main-axis size of a flex item is fixed (in the
      // constraint space). Make sure that this fixed size remains correct, by
      // adding cloned box decorations from each fragment.
      flex_item->main_axis_final_size += cloned_block_decorations;

      flex_item->total_remaining_block_size -=
          fragment.BlockSize() - cloned_block_decorations;
      if (flex_item->total_remaining_block_size < LayoutUnit() &&
          !physical_fragment.GetBreakToken()) {
        LayoutUnit expansion = -flex_item->total_remaining_block_size;
        flex_line.item_offset_adjustment += expansion;
      }
    } else if (!cross_size_adjustments_ &&
               !flex_item
                    ->has_descendant_that_depends_on_percentage_block_size) {
      // For rows, keep track of any expansion past the block-end of each
      // row so that we can re-run layout with the new row block-size.
      //
      // Include any cloned block-start box decorations. The line offset is in
      // the imaginary stitched container that we would have had had we not been
      // fragmented, and now we won't actual layout offsets for the current
      // fragment.
      LayoutUnit cloned_block_start_decoration =
          ClonedBlockStartDecoration(container_builder_);

      LayoutUnit line_block_end = flex_line.LineCrossEnd() -
                                  offset_in_stitched_container +
                                  cloned_block_start_decoration;
      if (line_block_end <= fragmentainer_space &&
          line_block_end >= LayoutUnit() &&
          offset_in_stitched_container != LayoutUnit::Max()) {
        LayoutUnit item_expansion;
        if (is_at_block_end) {
          item_expansion = item_block_end - line_block_end;
        } else {
          // We can't use the size of the fragment, as we don't
          // know how large the subsequent fragments will be (and how much
          // they'll expand the row).
          //
          // Instead of using the size of the fragment, expand the row to the
          // rest of the fragmentainer, with an additional epsilon. This epsilon
          // will ensure that we continue layout for children in this row in
          // the next fragmentainer. Without it we'd drop those subsequent
          // fragments.
          item_expansion = (fragmentainer_space - line_block_end).AddEpsilon();
        }

        // If the item expanded past the row, adjust any subsequent row offsets
        // to reflect the expansion.
        if (item_expansion > LayoutUnit()) {
          // Maps don't allow keys of 0, so adjust the index by 1.
          if (row_cross_size_updates_.empty() ||
              !row_cross_size_updates_.Contains(flex_line_idx + 1)) {
            row_cross_size_updates_.insert(flex_line_idx + 1, item_expansion);
            AdjustOffsetForNextLine(flex_lines, flex_line_idx, item_expansion);
          } else {
            auto it = row_cross_size_updates_.find(flex_line_idx + 1);
            CHECK_NE(it, row_cross_size_updates_.end());
            if (item_expansion > it->value) {
              AdjustOffsetForNextLine(flex_lines, flex_line_idx,
                                      item_expansion - it->value);
              it->value = item_expansion;
            }
          }
        }
      }
    }

    if (current_column_break_info) {
      DCHECK(is_column_);
      current_column_break_info->column_intrinsic_block_size =
          std::max(item_block_end,
                   current_column_break_info->column_intrinsic_block_size);
    }

    intrinsic_block_size_ = std::max(item_block_end, intrinsic_block_size_);
    container_builder_.AddResult(*layout_result, offset);
    if (current_column_break_info) {
      current_column_break_info->break_after =
          container_builder_.PreviousBreakAfter();
    }
    baseline_accumulator.AccumulateItem(fragment, offset.block_offset,
                                        is_first_line, is_last_line);
    if (last_item_in_line) {
      if (!has_inflow_child_break_inside_line[flex_line_idx])
        flex_line.has_seen_all_children = true;
      if (!has_processed_first_line_)
        has_processed_first_line_ = true;

      if (!physical_fragment.GetBreakToken() ||
          flex_line.has_seen_all_children) {
        if (flex_line_idx < flex_lines->size() - 1 && !is_column_ &&
            !item_iterator.HasMoreBreakTokens()) {
          // Add the offset adjustment of the current row to the next row so
          // that its items can also be adjusted by previous item expansion.
          // Only do this when the current row has completed layout and
          // the next row hasn't started layout yet.
          (*flex_lines)[flex_line_idx + 1].item_offset_adjustment +=
              flex_line.item_offset_adjustment;
        }
      }
    }
    last_line_idx_to_process_first_child_ = flex_line_idx;
  }

  if (needs_earlier_break_in_column ||
      status == LayoutResult::kNeedsEarlierBreak) {
    return LayoutResult::kNeedsEarlierBreak;
  }

  if (!row_cross_size_updates_.empty()) {
    DCHECK(!is_column_);
    return LayoutResult::kNeedsRelayoutWithRowCrossSizeChanges;
  }

  if (!container_builder_.HasInflowChildBreakInside() &&
      !item_iterator.NextItem(broke_before_row).flex_item) {
    container_builder_.SetHasSeenAllChildren();
  }

  if (auto first_baseline = baseline_accumulator.FirstBaseline())
    container_builder_.SetFirstBaseline(*first_baseline);
  if (auto last_baseline = baseline_accumulator.LastBaseline())
    container_builder_.SetLastBaseline(*last_baseline);

  // Update the |total_intrinsic_block_size_| in case things expanded.
  LayoutUnit updated_total_intrinsic_block_size =
      std::max(*total_intrinsic_block_size,
               intrinsic_block_size_ + previously_consumed_block_size);

  ipid_depth_log.FPrint(
      "[分片处理完成] 所有 flex 项目处理完成，准备返回结果：\n"
      "- 最终状态：{}\n"
      "- 基线信息已更新到容器构建器\n"
      "- 原始 total_intrinsic_block_size：{}，\n"
      "- 当前 intrinsic_block_size_：{}，previously_consumed_block_size：{}\n"
      "- 更新后的 total_intrinsic_block_size：{}",
      ipid::GetLayoutResultStatusString(static_cast<int>(status)),
      *total_intrinsic_block_size, intrinsic_block_size_,
      previously_consumed_block_size, updated_total_intrinsic_block_size);

  *total_intrinsic_block_size = updated_total_intrinsic_block_size;

  return status;
}

LayoutResult::EStatus FlexLayoutAlgorithm::PropagateFlexItemInfo(
    const FlexItem& flex_item,
    const PhysicalBoxFragment& physical_fragment,
    const PhysicalBoxStrut& physical_margins,
    wtf_size_t flex_line_idx,
    LogicalOffset offset) {
  LayoutResult::EStatus status = LayoutResult::kSuccess;

  if (layout_info_for_devtools_) [[unlikely]] {
    // If this is a "devtools layout", execution speed isn't critical but we
    // have to not adversely affect execution speed of a regular layout.
    PhysicalRect item_rect;
    item_rect.size = physical_fragment.Size();

    LogicalSize logical_flexbox_size =
        LogicalSize(container_builder_.InlineSize(), total_block_size_);
    PhysicalSize flexbox_size = ToPhysicalSize(
        logical_flexbox_size, GetConstraintSpace().GetWritingMode());
    item_rect.offset =
        offset.ConvertToPhysical(GetConstraintSpace().GetWritingDirection(),
                                 flexbox_size, item_rect.size);
    // devtools uses margin box.
    item_rect.Expand(physical_margins);
    DCHECK_GE(layout_info_for_devtools_->lines.size(), 1u);
    DevtoolsFlexInfo::Item item(item_rect,
                                BaselineAscent(flex_item, physical_fragment));
    layout_info_for_devtools_->lines[flex_line_idx].items.push_back(item);
  }

  // Detect if the flex-item had its scrollbar state change. If so we need
  // to relayout as the input to the flex algorithm is incorrect.
  if (!ignore_child_scrollbar_changes_) {
    if (flex_item.initial_scrollbars !=
        ComputeScrollbarsForNonAnonymous(flex_item.block_node)) {
      status = LayoutResult::kNeedsRelayoutWithNoChildScrollbarChanges;
    }

    // The flex-item scrollbars may not have changed, but an descendant's
    // scrollbars might have causing the min/max sizes to be incorrect.
    if (flex_item.depends_on_min_max_sizes &&
        flex_item.block_node.GetLayoutBox()->IntrinsicLogicalWidthsDirty()) {
      status = LayoutResult::kNeedsRelayoutWithNoChildScrollbarChanges;
    }
  } else {
    DCHECK_EQ(flex_item.initial_scrollbars,
              ComputeScrollbarsForNonAnonymous(flex_item.block_node));
  }
  return status;
}

void FlexLayoutAlgorithm::UpdateOffsetAdjustmentForSuppressedRowGap(
    LayoutUnit gap,
    LayoutUnit previous_content_block_end,
    FlexLine* flex_line) const {
  // Return early if there are no gaps specified since there will be nothing to
  // suppress.
  if (gap == LayoutUnit()) {
    return;
  }

  // Return early if we're in a fragmentainer with an unknown block size.
  if (!GetConstraintSpace().HasKnownFragmentainerBlockSize()) {
    return;
  }

  bool is_forced_break =
      To<BlockBreakToken>(container_builder_.LastChildBreakToken())
          ->IsForcedBreak();

  // Here, the current row or item could not fit in this fragmentainer, so we
  // want to suppress the gap that would appear at the start of the subsequent
  // fragmentainer. We'll factor this gap into the flex line's item offset
  // adjustment, allowing it to be applied during layout in the subsequent
  // fragmentainer.
  if (is_forced_break) {
    // For a forced break, the entire gap is deferred to the next fragmentainer,
    // so we subtract the full gap from the item offset adjustment.
    flex_line->item_offset_adjustment -= gap;
    return;
  }

  LayoutUnit available_space =
      FragmentainerSpaceAvailable(previous_content_block_end);
  // If the break isn't forced, part of the gap may have already been consumed
  // in this fragmentainer. We only suppress the unconsumed portion.
  if (gap > available_space) {
    // If the gap is larger than the available space, we need to adjust the
    // item offset adjustment to account for the unconsumed portion of the gap.
    // For row flex containers, the gap will always be greater than or equal to
    // the available space in a non-forced break scenario. This is because the
    // available space is based on the previous row's end.
    flex_line->item_offset_adjustment -= (gap - available_space);
  }

  // In column flex containers, we may encounter a case where the available
  // space is larger than the gap, yet an item still doesn't fit. In such
  // cases, the entire gap has already been consumed in this fragmentainer, so
  // no adjustment is needed. Adjustments should only be made when the gap
  // exceeds the available space which means that part of the gap may appear
  // in the next fragmentainer.
  // TODO(crbug.com/434735271): Determine if we can accurately CHECK that this
  // won't occur in a row-based flex container.
}

MinMaxSizesResult
FlexLayoutAlgorithm::ComputeMinMaxSizeOfMultilineColumnContainer() {
  IpidDepthLog ipid_depth_log(
      "FlexLayoutAlgorithm::ComputeMinMaxSizeOfMultilineColumnContainer");

  ipid_depth_log.FPrint(
      "开始计算纵向多行 flex 容器 {} 的固有宽度。\n"
      "这是一个特殊的算法，用于处理 flex-direction: column 且 flex-wrap: wrap "
      "的情况。",
      ipid::GetNodeStr(Node()));

  UseCounter::Count(Node().GetDocument(),
                    WebFeature::kFlexNewColumnWrapIntrinsicSize);
  ipid_depth_log.FPrint(
      "已记录 FlexNewColumnWrapIntrinsicSize 特性使用情况到 UseCounter。");

  MinMaxSizes min_max_sizes;
  // The algorithm for determining the max-content width of a column-wrap
  // container is simply: Run layout on the container but give the items an
  // overridden available size, equal to the largest max-content width of any
  // item, when they are laid out. The container's max-content width is then
  // the farthest outer inline-end point of all the items.
  ipid_depth_log.FPrint(
      "纵向多行 flex 容器的固有最大宽度计算算法：\n"
      "1. 运行容器布局，但给每个 flex item 一个覆盖的可用尺寸\n"
      "2. 该可用尺寸等于所有 item 中最大的 max-content 宽度\n"
      "3. 容器的 max-content 宽度就是所有 item 的最远外边界点");

  FlexLineVector flex_lines;
  ipid_depth_log.FPrint(
      "调用 PlaceFlexItems(Phase::kColumnWrapIntrinsicSize) "
      "来执行特殊的列包装布局...");
  PlaceFlexItems(Phase::kColumnWrapIntrinsicSize, &flex_lines);
  ipid_depth_log.FPrint("PlaceFlexItems 完成，生成了 {} 条 flex 行。",
                        flex_lines.size());

  ipid_depth_log.FPrint(
      "将固有最小宽度设置为 largest_min_content_contribution_: {}px\n"
      "这代表所有 flex item 中最大的 min-content 贡献值。",
      largest_min_content_contribution_);
  min_max_sizes.min_size = largest_min_content_contribution_;

  if (!flex_lines.empty()) {
    ipid_depth_log.FPrint("开始累加各行的交叉轴尺寸来计算固有最大宽度...");

    LayoutUnit total_cross_size;
    for (const auto& line : flex_lines) {
      ipid_depth_log.FPrint("行交叉轴尺寸: {}px", line.line_cross_size);
      total_cross_size += line.line_cross_size;
      min_max_sizes.max_size += line.line_cross_size;
    }

    LayoutUnit total_gap = (flex_lines.size() - 1) * gap_between_lines_;
    ipid_depth_log.FPrint(
        "行之间的 gap 计算：{} 行 - 1 = {} 个间隙，每个间隙 {}px，总计 {}px",
        flex_lines.size(), flex_lines.size() - 1, gap_between_lines_,
        total_gap);
    min_max_sizes.max_size += total_gap;

    ipid_depth_log.FPrint(
        "固有最大宽度 = 所有行的交叉轴尺寸之和 {}px + 行间隙 {}px = {}px",
        total_cross_size, total_gap, min_max_sizes.max_size);
  } else {
    ipid_depth_log.FPrint("没有 flex 行生成，固有最大宽度保持为 0。");
  }

  ipid_depth_log.FPrint(
      "验证计算结果的合理性：\n"
      "固有最小宽度 {}px >= 0: {}\n"
      "固有最小宽度 {}px <= 固有最大宽度 {}px: {}",
      min_max_sizes.min_size, ipid::btos(min_max_sizes.min_size >= 0),
      min_max_sizes.min_size, min_max_sizes.max_size,
      ipid::btos(min_max_sizes.min_size <= min_max_sizes.max_size));
  DCHECK_GE(min_max_sizes.min_size, 0);
  DCHECK_LE(min_max_sizes.min_size, min_max_sizes.max_size);

  LayoutUnit border_scrollbar_padding_inline =
      BorderScrollbarPadding().InlineSum();
  ipid_depth_log.FPrint(
      "加上容器自身的 border + scrollbar + padding 横向值: {}px",
      border_scrollbar_padding_inline);
  min_max_sizes += border_scrollbar_padding_inline;

  // This always depends on block constraints because if block constraints
  // change, this flexbox could get a different number of columns.
  ipid_depth_log.FPrint(
      "纵向多行 flex 容器的固有宽度总是依赖于块级约束，\n"
      "因为块级约束的改变可能会导致不同数量的列。\n"
      "最终计算结果：固有宽度 {}，depends_on_block_constraints: true",
      ipid::GetMinMaxSizesString(min_max_sizes));

  return {min_max_sizes, /* depends_on_block_constraints */ true};
}

MinMaxSizesResult FlexLayoutAlgorithm::ComputeMinMaxSizeOfRowContainer() {
  IpidDepthLog ipid_depth_log(
      "FlexLayoutAlgorithm::ComputeMinMaxSizeOfRowContainer");

  ipid_depth_log.FPrint(
      "正在计算行方向 flex 容器的固有宽度。\n"
      "当前 flex 容器是否为多行模式：{}\n"
      "当前 flex 容器是否为水平流动：{}",
      ipid::btos(is_multi_line_), ipid::btos(is_horizontal_flow_));

  MinMaxSizes container_sizes;
  bool depends_on_block_constraints = false;

  // The intrinsic sizing algorithm uses lots of geometry and values from each
  // item (e.g. flex base size, used minimum and maximum sizes including
  // automatic minimum sizing), so re-use |ConstructAndAppendFlexItems| from the
  // layout algorithm, which calculates all that.
  // TODO(dgrogan): As an optimization, We can drop the call to
  // ComputeMinMaxSizes in |ConstructAndAppendFlexItems| during this phase if
  // the flex basis is not definite.
  ipid_depth_log.FPrint(
      "调用 ConstructAndAppendFlexItems(Phase::kRowIntrinsicSize) "
      "来构建所有 flex 项目的基础信息（包括 flex base size、min/max "
      "sizes 等），这些信息将用于后续计算每个项目对容器固有宽度的贡献值。");
  ConstructAndAppendFlexItems(Phase::kRowIntrinsicSize);

  ipid_depth_log.FPrint(
      "构建完成，共有 {} 个 flex "
      "项目。现在开始遍历每个项目，计算其对容器固有宽度的贡献值。",
      flex_items_.size());

  LayoutUnit largest_outer_min_content_contribution;
  for (const FlexItem& item : flex_items_) {
    const BlockNode& child = item.block_node;

    ipid_depth_log.FPrint(
        "\n正在处理第 {} 个 flex 项目：{}\n"
        "该项目的 flex-grow: {}，flex-shrink: {}\n"
        "该项目的 flex base size: {}px（不含 border/padding）\n"
        "该项目的 hypothetical size: {}px（不含 border/padding）\n"
        "该项目的 main axis border+padding: {}px\n"
        "该项目是否使用了不确定的 flex basis：{}",
        item.item_index, ipid::GetNodeStr(child), item.flex_grow,
        item.flex_shrink, item.base_content_size,
        item.hypothetical_content_size, item.main_axis_border_padding,
        ipid::btos(item.is_used_flex_basis_indefinite));

    const ConstraintSpace space =
        BuildSpaceForIntrinsicInlineSize(child, item.alignment);
    ipid_depth_log.FPrint(
        "为该项目构建用于计算固有宽度贡献值的 "
        "ConstraintSpace，考虑了该项目的对齐方式：{}",
        ipid::GetConstraintSpaceString(space));

    const MinMaxSizesResult min_max_content_contributions =
        ComputeMinAndMaxContentContribution(Style(), child, space);
    depends_on_block_constraints |=
        min_max_content_contributions.depends_on_block_constraints;

    ipid_depth_log.FPrint(
        "计算得到该项目的固有宽度贡献值：{}",
        ipid::GetMinMaxSizesResultString(min_max_content_contributions));

    MinMaxSizes item_final_contribution;
    const LayoutUnit flex_base_size_border_box =
        item.base_content_size + item.main_axis_border_padding;
    const LayoutUnit hypothetical_main_size_border_box =
        item.hypothetical_content_size + item.main_axis_border_padding;

    ipid_depth_log.FPrint(
        "计算该项目的关键尺寸值：\n"
        "flex base size（含 border/padding）: {}px\n"
        "hypothetical main size（含 border/padding）: {}px",
        flex_base_size_border_box, hypothetical_main_size_border_box);

    const LayoutUnit main_axis_margins =
        is_horizontal_flow_ ? item.initial_margins.HorizontalSum()
                            : item.initial_margins.VerticalSum();

    ipid_depth_log.FPrint("该项目在主轴方向的 margin 总值为：{}px",
                          main_axis_margins);

    if (is_multi_line_) {
      ipid_depth_log.FPrint(
          "当前为多行 flex "
          "容器。对于多行容器，我们需要找出所有项目中最大的固有最小宽度贡献值"
          "（包含 "
          "margin），因为多行容器的固有最小宽度等于能容纳最宽项目的宽度。");

      const LayoutUnit outer_min_contribution =
          min_max_content_contributions.sizes.min_size + main_axis_margins;

      ipid_depth_log.FPrint(
          "该项目的外层固有最小宽度贡献值（含 margin）为：{}px，"
          "当前记录的最大值为：{}px",
          outer_min_contribution, largest_outer_min_content_contribution);

      largest_outer_min_content_contribution = std::max(
          largest_outer_min_content_contribution, outer_min_contribution);

      ipid_depth_log.FPrint("更新后的最大外层固有最小宽度贡献值为：{}px",
                            largest_outer_min_content_contribution);
    } else {
      ipid_depth_log.FPrint(
          "当前为单行 flex 容器。对于单行容器，需要考虑每个项目是否能够伸缩，"
          "来决定其对容器固有最小宽度的最终贡献值。");

      const LayoutUnit min_contribution =
          min_max_content_contributions.sizes.min_size;

      // Note: |cant_move| is not actually necessary to pass the compat cases
      // that have broke in the past, but it does restrict the new algorithm to
      // a smaller set of scenarios where the old algorithm was egregiously
      // wrong. If this version of the algorithm IS web compatible, we can then
      // try removing the cant_move requirement.
      const bool cant_move = (min_contribution > flex_base_size_border_box &&
                              item.flex_grow == 0.f) ||
                             (min_contribution < flex_base_size_border_box &&
                              item.flex_shrink == 0.f);

      ipid_depth_log.FPrint(
          "该项目的固有最小宽度贡献值为：{}px\n"
          "判断该项目是否无法伸缩（cant_move）：\n"
          "- 条件1：固有最小宽度 > flex base size 且 flex-grow = 0：{}\n"
          "- 条件2：固有最小宽度 < flex base size 且 flex-shrink = 0：{}\n"
          "cant_move 结果：{}",
          min_contribution,
          ipid::btos(min_contribution > flex_base_size_border_box &&
                     item.flex_grow == 0.f),
          ipid::btos(min_contribution < flex_base_size_border_box &&
                     item.flex_shrink == 0.f),
          ipid::btos(cant_move));

      // Note: We could further restrict the new algorithm to only apply to
      // items that have both a fixed flex basis AND do not use automatic
      // minimum sizing AND whose min and max properties do not depend on the
      // item's content (e.g. fit-content, max-content etc). But last time we
      // enabled this algorithm there were no bugs filed, so hopefully those
      // further restrictions are not necessary. If we have compat problems this
      // iteration, we can see if any would be fixed by employing such
      // restrictions.
      if (cant_move && !item.is_used_flex_basis_indefinite) {
        item_final_contribution.min_size = hypothetical_main_size_border_box;
        ipid_depth_log.FPrint(
            "该项目无法伸缩且使用确定的 flex "
            "basis，因此其固有最小宽度贡献值使用 "
            "hypothetical main size: {}px",
            hypothetical_main_size_border_box);
      } else {
        item_final_contribution.min_size = min_contribution;
        ipid_depth_log.FPrint(
            "该项目可以伸缩或使用不确定的 flex "
            "basis，因此其固有最小宽度贡献值使用原始计算值: {}px",
            min_contribution);
      }
    }

    ipid_depth_log.FPrint("现在计算该项目对容器固有最大宽度的贡献值。");

    const LayoutUnit max_contribution =
        min_max_content_contributions.sizes.max_size;
    const bool cant_move_max = (max_contribution > flex_base_size_border_box &&
                                item.flex_grow == 0.f) ||
                               (max_contribution < flex_base_size_border_box &&
                                item.flex_shrink == 0.f);

    ipid_depth_log.FPrint(
        "该项目的固有最大宽度贡献值为：{}px\n"
        "判断该项目是否无法伸缩（cant_move for max）：\n"
        "- 条件1：固有最大宽度 > flex base size 且 flex-grow = 0：{}\n"
        "- 条件2：固有最大宽度 < flex base size 且 flex-shrink = 0：{}\n"
        "cant_move 结果：{}",
        max_contribution,
        ipid::btos(max_contribution > flex_base_size_border_box &&
                   item.flex_grow == 0.f),
        ipid::btos(max_contribution < flex_base_size_border_box &&
                   item.flex_shrink == 0.f),
        ipid::btos(cant_move_max));

    if (cant_move_max && !item.is_used_flex_basis_indefinite) {
      item_final_contribution.max_size = hypothetical_main_size_border_box;
      ipid_depth_log.FPrint(
          "该项目无法伸缩且使用确定的 flex basis，因此其固有最大宽度贡献值使用 "
          "hypothetical main size: {}px",
          hypothetical_main_size_border_box);
    } else {
      item_final_contribution.max_size = max_contribution;
      ipid_depth_log.FPrint(
          "该项目可以伸缩或使用不确定的 flex "
          "basis，因此其固有最大宽度贡献值使用原始计算值: {}px",
          max_contribution);
    }

    ipid_depth_log.FPrint(
        "该项目的最终贡献值为：{}\n"
        "将该贡献值和 margin（{}px）累加到容器尺寸中。",
        ipid::GetMinMaxSizesString(item_final_contribution), main_axis_margins);

    container_sizes += item_final_contribution;
    container_sizes += main_axis_margins;

    ipid_depth_log.FPrint("累加后容器的当前固有宽度为：{}",
                          ipid::GetMinMaxSizesString(container_sizes));
  }

  ipid_depth_log.FPrint(
      "完成所有项目的遍历。现在处理项目间的间隙（gap）和多行/"
      "单行容器的不同逻辑。");

  if (!flex_items_.empty()) {
    const LayoutUnit gap_inline_size =
        (flex_items_.size() - 1) * gap_between_items_;

    ipid_depth_log.FPrint(
        "计算项目间的总间隙：项目数量为 {}，gap_between_items_ 为 {}px，"
        "因此总间隙为 {}px",
        flex_items_.size(), gap_between_items_, gap_inline_size);

    if (is_multi_line_) {
      ipid_depth_log.FPrint(
          "多行容器的处理：\n"
          "- 固有最小宽度设为最大的外层最小宽度贡献值：{}px\n"
          "- 固有最大宽度在当前值基础上加上间隙：{} + {} = {}px",
          largest_outer_min_content_contribution, container_sizes.max_size,
          gap_inline_size, container_sizes.max_size + gap_inline_size);

      container_sizes.min_size = largest_outer_min_content_contribution;
      container_sizes.max_size += gap_inline_size;
    } else {
      DCHECK_EQ(largest_outer_min_content_contribution, LayoutUnit())
          << "largest_outer_min_content_contribution is not filled in for "
             "singleline containers.";

      ipid_depth_log.FPrint(
          "单行容器的处理：在当前的固有宽度值 {} 基础上都加上间隙 {}px",
          ipid::GetMinMaxSizesString(container_sizes), gap_inline_size);

      container_sizes += gap_inline_size;
    }

    ipid_depth_log.FPrint("处理间隙后的容器固有宽度为：{}",
                          ipid::GetMinMaxSizesString(container_sizes));
  } else {
    ipid_depth_log.FPrint("没有 flex 项目，不需要处理间隙。");
  }

  // Handle potential weirdness caused by items' negative margins.
  ipid_depth_log.FPrint("进行最终调整，处理可能由负 margin 引起的异常情况。");

#if DCHECK_IS_ON()
  if (container_sizes.max_size < container_sizes.min_size) {
    DCHECK(is_multi_line_)
        << container_sizes
        << " multiline row containers might have max < min due to negative "
           "margins, but singleline containers cannot.";

    ipid_depth_log.FPrint(
        "检测到固有最大宽度 < 固有最小宽度的情况，这可能是由负 margin 导致的。"
        "（这种情况只有在多行容器中才可能出现）");
  }
#endif

  const LayoutUnit original_max_size = container_sizes.max_size;
  container_sizes.max_size =
      std::max(container_sizes.max_size, container_sizes.min_size);

  if (original_max_size != container_sizes.max_size) {
    ipid_depth_log.FPrint(
        "调整固有最大宽度：从 {}px 调整为 {}px，确保最大宽度不小于最小宽度",
        original_max_size, container_sizes.max_size);
  }

  container_sizes.Encompass(LayoutUnit());
  ipid_depth_log.FPrint("确保固有宽度不小于 0：{}",
                        ipid::GetMinMaxSizesString(container_sizes));

  const BoxStrut border_scrollbar_padding = BorderScrollbarPadding();
  ipid_depth_log.FPrint(
      "加上容器自身的 border + scrollbar + padding 的宽度值：{}px",
      border_scrollbar_padding.InlineSum());

  container_sizes += border_scrollbar_padding.InlineSum();

  ipid_depth_log.FPrint(
      "最终的行方向 flex 容器固有宽度计算结果：{}\n"
      "是否依赖于高度约束：{}",
      ipid::GetMinMaxSizesString(container_sizes),
      ipid::btos(depends_on_block_constraints));

  return MinMaxSizesResult(container_sizes, depends_on_block_constraints);
}

MinMaxSizesResult FlexLayoutAlgorithm::ComputeMinMaxSizes(
    const MinMaxSizesFloatInput&) {
  IpidDepthLog ipid_depth_log(
      "flex_layout_algorithm.cc: FlexLayoutAlgorithm::ComputeMinMaxSizes");

  ipid_depth_log.FPrint(
      "正在计算 Flex 容器 {} 的固有宽度。\n"
      "is_column_: {}（是否为纵向 flex）\n"
      "is_multi_line_: {}（是否为多行 flex）\n"
      "BorderScrollbarPadding: {}",
      ipid::GetNodeStr(Node()), ipid::btos(is_column_),
      ipid::btos(is_multi_line_),
      ipid::GetBoxStrutString(BorderScrollbarPadding()));

  if (auto result = CalculateMinMaxSizesIgnoringChildren(
          Node(), BorderScrollbarPadding())) {
    ipid_depth_log.FPrint(
        "Flex 容器 {} 无需考虑子元素即可确定固有宽度，直接返回结果 {}。",
        ipid::GetNodeStr(Node()), ipid::GetMinMaxSizesResultString(*result));
    return *result;
  }

  if (is_column_ && is_multi_line_) {
    ipid_depth_log.FPrint(
        "当前是纵向多行 flex 容器，调用专用的 "
        "ComputeMinMaxSizeOfMultilineColumnContainer 算法。");
    return ComputeMinMaxSizeOfMultilineColumnContainer();
  }

  if (RuntimeEnabledFeatures::LayoutFlexNewRowAlgorithmEnabled() &&
      !is_column_) {
    ipid_depth_log.FPrint(
        "当前是横向 flex 容器且开启了 LayoutFlexNewRowAlgorithm "
        "特性，调用专用的 ComputeMinMaxSizeOfRowContainer 算法。");
    return ComputeMinMaxSizeOfRowContainer();
  }

  ipid_depth_log.FPrint(
      "Flex 容器需要遍历所有子元素来计算固有宽度，开始逐个处理 flex item。");

  MinMaxSizes sizes;
  bool depends_on_block_constraints = false;

  int number_of_items = 0;
  FlexChildIterator iterator(Node());
  for (BlockNode child = iterator.NextChild(); child;
       child = iterator.NextChild()) {
    if (child.IsOutOfFlowPositioned()) {
      ipid_depth_log.FPrint(
          "跳过绝对定位的子元素 {}，因为它不参与 flex 布局的固有宽度计算。",
          ipid::GetNodeStr(child));
      continue;
    }
    number_of_items++;

    ipid_depth_log.FPrint("正在处理第 {} 个 flex item：{}", number_of_items,
                          ipid::GetNodeStr(child));

    ItemPosition alignment = ResolvedAlignSelf(child.Style());
    ipid_depth_log.FPrint("该子元素的 align-self 解析结果为：{}",
                          ipid::GetItemPositionString(alignment));

    const ConstraintSpace space =
        BuildSpaceForIntrinsicInlineSize(child, alignment);
    ipid_depth_log.FPrint(
        "为计算子元素固有宽度贡献值构建的 ConstraintSpace：{}",
        ipid::GetConstraintSpaceString(space));

    MinMaxSizesResult child_result =
        ComputeMinAndMaxContentContribution(Style(), child, space);
    ipid_depth_log.FPrint("子元素 {} 的固有宽度贡献值为：{}",
                          ipid::GetNodeStr(child),
                          ipid::GetMinMaxSizesResultString(child_result));

    BoxStrut child_margins =
        ComputeMarginsFor(space, child.Style(), GetConstraintSpace());
    ipid_depth_log.FPrint("子元素 {} 的 margin 值为：{}",
                          ipid::GetNodeStr(child),
                          ipid::GetBoxStrutString(child_margins));

    child_result.sizes += child_margins.InlineSum();
    ipid_depth_log.FPrint("加上 margin 后，子元素 {} 的最终贡献值为：{}",
                          ipid::GetNodeStr(child),
                          ipid::GetMinMaxSizesString(child_result.sizes));

    depends_on_block_constraints |= child_result.depends_on_block_constraints;
    if (is_column_) {
      LayoutUnit old_min = sizes.min_size;
      LayoutUnit old_max = sizes.max_size;
      sizes.min_size = std::max(sizes.min_size, child_result.sizes.min_size);
      sizes.max_size = std::max(sizes.max_size, child_result.sizes.max_size);
      ipid_depth_log.FPrint(
          "纵向 flex：取各子元素贡献值的最大值。\n"
          "之前的固有宽度为 {}，子元素贡献 {}，\n"
          "更新后的固有宽度为 {}",
          ipid::GetMinMaxSizesString({old_min, old_max}),
          ipid::GetMinMaxSizesString(child_result.sizes),
          ipid::GetMinMaxSizesString(sizes));
    } else {
      LayoutUnit old_min = sizes.min_size;
      LayoutUnit old_max = sizes.max_size;
      sizes.max_size += child_result.sizes.max_size;
      if (is_multi_line_) {
        sizes.min_size = std::max(sizes.min_size, child_result.sizes.min_size);
        ipid_depth_log.FPrint(
            "横向多行 "
            "flex：固有最大宽度累加子元素贡献值，固有最小宽度取各子元素的最大值"
            "。\n"
            "之前的固有宽度为 {}，子元素贡献 {}，\n"
            "更新后的固有宽度为 {}",
            ipid::GetMinMaxSizesString({old_min, old_max}),
            ipid::GetMinMaxSizesString(child_result.sizes),
            ipid::GetMinMaxSizesString(sizes));
      } else {
        sizes.min_size += child_result.sizes.min_size;
        ipid_depth_log.FPrint(
            "横向单行 flex：固有最大宽度和最小宽度都累加子元素贡献值。\n"
            "之前的固有宽度为 {}，子元素贡献 {}，\n"
            "更新后的固有宽度为 {}",
            ipid::GetMinMaxSizesString({old_min, old_max}),
            ipid::GetMinMaxSizesString(child_result.sizes),
            ipid::GetMinMaxSizesString(sizes));
      }
    }
  }

  ipid_depth_log.FPrint("处理完所有 {} 个 flex item 后，基础固有宽度为：{}",
                        number_of_items, ipid::GetMinMaxSizesString(sizes));

  if (!is_column_ && number_of_items > 0) {
    LayoutUnit gap_inline_size = (number_of_items - 1) * gap_between_items_;
    ipid_depth_log.FPrint(
        "横向 flex 容器需要计算 gap 的影响。\n"
        "gap_between_items_: {}，item 数量: {}，总 gap 宽度: {}",
        gap_between_items_, number_of_items, gap_inline_size);

    LayoutUnit old_min = sizes.min_size;
    LayoutUnit old_max = sizes.max_size;
    sizes.max_size += gap_inline_size;
    if (!is_multi_line_) {
      sizes.min_size += gap_inline_size;
      ipid_depth_log.FPrint(
          "横向单行 flex：固有最大宽度和最小宽度都要加上 gap 宽度。\n"
          "之前：{}，加上 gap {} 后：{}",
          ipid::GetMinMaxSizesString({old_min, old_max}), gap_inline_size,
          ipid::GetMinMaxSizesString(sizes));
    } else {
      ipid_depth_log.FPrint(
          "横向多行 flex：只有固有最大宽度需要加上 gap 宽度。\n"
          "之前：{}，加上 gap {} 后：{}",
          ipid::GetMinMaxSizesString({old_min, old_max}), gap_inline_size,
          ipid::GetMinMaxSizesString(sizes));
    }
  }
  ipid_depth_log.FPrint(
      "调整固有最大宽度，保证其 >= "
      "固有最小宽度（即将右边的数字设为 >= 左边的数字）。调整前的固有宽度：{}",
      ipid::GetMinMaxSizesString(sizes));
  sizes.max_size = std::max(sizes.max_size, sizes.min_size);
  ipid_depth_log.FPrint("调整后的固有宽度：{}",
                        ipid::GetMinMaxSizesString(sizes));

  // Due to negative margins, it is possible that we calculated a negative
  // intrinsic width. Make sure that we never return a negative width.
  ipid_depth_log.FPrint(
      "由于当元素有负 margin "
      "时，可能导致固有宽度出现负值，需要确保固有宽度不为负值。之前：{}",
      ipid::GetMinMaxSizesString(sizes));
  sizes.Encompass(LayoutUnit());
  ipid_depth_log.FPrint("处理负值后：{}", ipid::GetMinMaxSizesString(sizes));

  LayoutUnit border_scrollbar_padding_inline =
      BorderScrollbarPadding().InlineSum();
  ipid_depth_log.FPrint(
      "最后需要加上 Flex 容器自身的 border + scrollbar + padding 宽度：{}px",
      border_scrollbar_padding_inline);
  sizes += border_scrollbar_padding_inline;

  MinMaxSizesResult final_result(sizes, depends_on_block_constraints);
  ipid_depth_log.FPrint(
      "Flex 容器 {} 的最终固有宽度计算结果：{}\n"
      "depends_on_block_constraints: {}",
      ipid::GetNodeStr(Node()), ipid::GetMinMaxSizesString(final_result.sizes),
      ipid::btos(depends_on_block_constraints));

  return final_result;
}

LayoutUnit FlexLayoutAlgorithm::FragmentainerSpaceAvailable(
    LayoutUnit block_offset) const {
  return (FragmentainerSpaceLeftForChildren() - block_offset)
      .ClampNegativeToZero();
}

void FlexLayoutAlgorithm::ConsumeRemainingFragmentainerSpace(
    LayoutUnit offset_in_stitched_container,
    FlexLine* flex_line,
    const FlexColumnBreakInfo* column_break_info) {
  if (To<BlockBreakToken>(container_builder_.LastChildBreakToken())
          ->IsForcedBreak()) {
    // This will be further adjusted by the total consumed block size once we
    // handle the break before in the next fragmentainer. This ensures that the
    // expansion is properly handled in the column balancing pass.
    LayoutUnit intrinsic_block_size = intrinsic_block_size_;
    if (column_break_info) {
      DCHECK(is_column_);
      intrinsic_block_size = column_break_info->column_intrinsic_block_size;
    }

    // Any cloned block-start box decorations shouldn't count here, since we're
    // calculating an offset into the imaginary stitched container that we would
    // have had had we not been fragmented. The space taken up by a cloned
    // border is unavailable to child content (flex items in this case).
    LayoutUnit cloned_block_start_decoration =
        ClonedBlockStartDecoration(container_builder_);

    flex_line->item_offset_adjustment -= intrinsic_block_size +
                                         offset_in_stitched_container -
                                         cloned_block_start_decoration;
  }

  if (!GetConstraintSpace().HasKnownFragmentainerBlockSize()) {
    return;
  }
  // The remaining part of the fragmentainer (the unusable space for child
  // content, due to the break) should still be occupied by this container.
  intrinsic_block_size_ += FragmentainerSpaceAvailable(intrinsic_block_size_);
}

BreakStatus FlexLayoutAlgorithm::BreakBeforeRowIfNeeded(
    const FlexLine& row,
    LayoutUnit row_block_offset,
    EBreakBetween row_break_between,
    wtf_size_t row_index,
    LayoutInputNode child,
    bool has_container_separation,
    bool is_first_for_row) {
  DCHECK(!is_column_);
  DCHECK(InvolvedInBlockFragmentation(container_builder_));

  LayoutUnit fragmentainer_block_offset =
      FragmentainerOffsetForChildren() + row_block_offset;
  LayoutUnit fragmentainer_block_size = FragmentainerCapacityForChildren();

  if (has_container_separation) {
    if (IsForcedBreakValue(GetConstraintSpace(), row_break_between)) {
      BreakBeforeChild(GetConstraintSpace(), child, /*layout_result=*/nullptr,
                       fragmentainer_block_offset, fragmentainer_block_size,
                       kBreakAppealPerfect, /*is_forced_break=*/true,
                       &container_builder_, row.line_cross_size);
      return BreakStatus::kBrokeBefore;
    }
  }

  bool breakable_at_start_of_container = IsBreakableAtStartOfResumedContainer(
      GetConstraintSpace(), container_builder_, is_first_for_row);
  BreakAppeal appeal_before = CalculateBreakAppealBefore(
      GetConstraintSpace(), LayoutResult::EStatus::kSuccess, row_break_between,
      has_container_separation, breakable_at_start_of_container);

  // Attempt to move past the break point, and if we can do that, also assess
  // the appeal of breaking there, even if we didn't.
  if (MovePastRowBreakPoint(
          appeal_before, fragmentainer_block_offset, row.line_cross_size,
          row_index, has_container_separation, breakable_at_start_of_container))
    return BreakStatus::kContinue;

  // We're out of space. Figure out where to insert a soft break. It will either
  // be before this row, or before an earlier sibling, if there's a more
  // appealing breakpoint there.
  if (!AttemptSoftBreak(GetConstraintSpace(), child,
                        /*layout_result=*/nullptr, fragmentainer_block_offset,
                        fragmentainer_block_size, appeal_before,
                        &container_builder_, row.line_cross_size)) {
    return BreakStatus::kNeedsEarlierBreak;
  }

  return BreakStatus::kBrokeBefore;
}

bool FlexLayoutAlgorithm::MovePastRowBreakPoint(
    BreakAppeal appeal_before,
    LayoutUnit fragmentainer_block_offset,
    LayoutUnit row_block_size,
    wtf_size_t row_index,
    bool has_container_separation,
    bool breakable_at_start_of_container) {
  if (!GetConstraintSpace().HasKnownFragmentainerBlockSize()) {
    // We only care about soft breaks if we have a fragmentainer block-size.
    // During column balancing this may be unknown.
    return true;
  }

  LayoutUnit space_left =
      FragmentainerCapacityForChildren() - fragmentainer_block_offset;

  // If the row starts past the end of the fragmentainer, we must break before
  // it.
  bool must_break_before = false;
  if (space_left < LayoutUnit()) {
    must_break_before = true;
  } else if (space_left == LayoutUnit()) {
    // If the row starts exactly at the end, we'll allow the row here if the
    // row has zero block-size. Otherwise we have to break before it.
    must_break_before = row_block_size != LayoutUnit();
  }
  if (must_break_before) {
#if DCHECK_IS_ON()
    bool refuse_break_before = space_left >= FragmentainerCapacityForChildren();
    DCHECK(!refuse_break_before);
#endif
    return false;
  }

  // Update the early break in case breaking before the row ends up being the
  // most appealing spot to break.
  if ((has_container_separation || breakable_at_start_of_container) &&
      (!container_builder_.HasEarlyBreak() ||
       appeal_before >= container_builder_.GetEarlyBreak().GetBreakAppeal())) {
    container_builder_.SetEarlyBreak(
        MakeGarbageCollected<EarlyBreak>(row_index, appeal_before));
  }

  // Avoiding breaks inside a row will be handled at the item level.
  return true;
}

void FlexLayoutAlgorithm::AddColumnEarlyBreak(EarlyBreak* breakpoint,
                                              wtf_size_t index) {
  DCHECK(is_column_);
  while (column_early_breaks_.size() <= index)
    column_early_breaks_.push_back(nullptr);
  column_early_breaks_[index] = breakpoint;
}

void FlexLayoutAlgorithm::AdjustOffsetForNextLine(
    FlexLineVector* flex_lines,
    wtf_size_t flex_line_idx,
    LayoutUnit item_expansion) const {
  DCHECK_LT(flex_line_idx, flex_lines->size());
  if (flex_line_idx == flex_lines->size() - 1) {
    return;
  }
  (*flex_lines)[flex_line_idx + 1].item_offset_adjustment += item_expansion;
}

const LayoutResult* FlexLayoutAlgorithm::RelayoutWithNewRowSizes() {
  // We shouldn't update the row cross-sizes more than once per fragmentainer.
  DCHECK(!cross_size_adjustments_);

  // There should be no more than two row expansions per fragmentainer.
  DCHECK(!row_cross_size_updates_.empty());
  DCHECK_LE(row_cross_size_updates_.size(), 2u);

  LayoutAlgorithmParams params(Node(),
                               container_builder_.InitialFragmentGeometry(),
                               GetConstraintSpace(), GetBreakToken(),
                               early_break_, additional_early_breaks_);
  FlexLayoutAlgorithm algorithm_with_row_cross_sizes(params,
                                                     &row_cross_size_updates_);
  auto& new_builder = algorithm_with_row_cross_sizes.container_builder_;
  new_builder.SetBoxType(container_builder_.GetBoxType());
  algorithm_with_row_cross_sizes.ignore_child_scrollbar_changes_ =
      ignore_child_scrollbar_changes_;

  // We may have aborted layout due to an early break previously. Ensure that
  // the builder detects the correct space shortage, if so.
  if (early_break_) {
    new_builder.PropagateSpaceShortage(
        container_builder_.MinimalSpaceShortage());
  }
  return algorithm_with_row_cross_sizes.Layout();
}

// We are interested in cases where the flex item *may* expand due to
// fragmentation (lines pushed down by a fragmentation line, etc).
bool FlexLayoutAlgorithm::MinBlockSizeShouldEncompassIntrinsicSize(
    const FlexItemData& item) const {
  // If this item has (any) descendant that is percentage based, we can end
  // up in a situation where we'll constantly try and expand the row. E.g.
  // <div style="display: flex;">
  //   <div style="min-height: 100px;">
  //     <div style="height: 200%;"></div>
  //   </div>
  // </div>
  if (item.has_descendant_that_depends_on_percentage_block_size)
    return false;

  if (item.block_node.IsMonolithic()) {
    return false;
  }

  const auto& item_style = item.block_node.Style();

  // NOTE: We currently assume that writing-mode roots are monolithic, but
  // this may change in the future.
  DCHECK_EQ(GetConstraintSpace().GetWritingDirection().GetWritingMode(),
            item_style.GetWritingMode());

  if (is_column_) {
    bool can_shrink = item_style.ResolvedFlexShrink(Style()) != 0.f &&
                      ChildAvailableSize().block_size != kIndefiniteSize;

    // Only allow growth if the item can't shrink and the flex-basis is
    // content-based.
    if (item.is_used_flex_basis_indefinite && !can_shrink) {
      return true;
    }

    // Only allow growth if the item's block-size is auto and either the item
    // can't shrink or its min-height is auto.
    if (item_style.LogicalHeight().HasAutoOrContentOrIntrinsic() &&
        (!can_shrink || ShouldApplyAutoMinSize(item.block_node))) {
      return true;
    }
  } else {
    // Don't grow if the item's block-size should be the same as its container.
    if (WillChildCrossSizeBeContainerCrossSize(item.block_node,
                                               item.alignment) &&
        !Style().LogicalHeight().HasAutoOrContentOrIntrinsic()) {
      return false;
    }

    // Only allow growth if the item's cross size is auto.
    if (DoesItemComputedCrossSizeHaveAuto(item.block_node)) {
      return true;
    }
  }
  return false;
}

}  // namespace blink
