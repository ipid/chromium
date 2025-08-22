// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/layout/length_utils.h"

#include <algorithm>
#include <optional>

#include "third_party/blink/renderer/core/html/html_element.h"
#include "third_party/blink/renderer/core/layout/block_node.h"
#include "third_party/blink/renderer/core/layout/constraint_space.h"
#include "third_party/blink/renderer/core/layout/constraint_space_builder.h"
#include "third_party/blink/renderer/core/layout/fragmentation_utils.h"
#include "third_party/blink/renderer/core/layout/geometry/box_strut.h"
#include "third_party/blink/renderer/core/layout/geometry/logical_size.h"
#include "third_party/blink/renderer/core/layout/layout_box.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/layout/space_utils.h"
#include "third_party/blink/renderer/core/layout/svg/layout_svg_root.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/platform/geometry/layout_unit.h"
#include "third_party/blink/renderer/platform/geometry/length.h"
#include "third_party/blink/renderer/platform/geometry/length_functions.h"

// ------ ipid logging START ------
#include "third_party/blink/renderer/core/layout/ipid_debug_layout_str_utils.h"
#include "third_party/blink/renderer/platform/ipid_logging/ipid_depth_logging.h"
// ------ ipid logging END ------

namespace blink {

LayoutUnit ResolveInlineLengthInternal(
    const ConstraintSpace& constraint_space,
    const ComputedStyle& style,
    const BoxStrut& border_padding,
    MinMaxSizesFunctionRef min_max_sizes_func,
    const Length& original_length,
    const Length* auto_length,
    LengthTypeInternal length_type,
    FitContentMode fit_content_mode,
    LayoutUnit override_available_size,
    CalcSizeKeywordBehavior calc_size_keyword_behavior) {
  IpidDepthLog ipid_depth_log("length_utils.cc: ResolveInlineLengthInternal");

  DCHECK_EQ(constraint_space.GetWritingMode(), style.GetWritingMode());

  ipid_depth_log.FPrint(
      "正在解析宽度 {}，\n开辟的空间：{}\n上游需要解析哪种宽度：{}\n当前元素的 "
      "border+padding：{}",
      original_length, ipid::GetConstraintSpaceString(constraint_space),
      ipid::GetLengthTypeInternalString(length_type),
      ipid::GetBoxStrutString(border_padding));

  // For min-inline-size, this might still be 'auto'.
  const Length& length =
      original_length.IsAuto() && auto_length ? *auto_length : original_length;

  if (original_length.IsAuto() && length.IsAuto()) {
    ipid_depth_log.FPrint(
        "当前要解析的宽度为 auto，但上游没有传 "
        "auto_length（可以理解为一个兜底值，当宽度为 auto "
        "时兜底为此值），因此后续将直接对 "
        "auto 值作解析。");
  } else if (original_length.IsAuto() && !length.IsAuto()) {
    ipid_depth_log.FPrint(
        "当前要解析的宽度为 auto，但上游传入了 auto_length = "
        "{}（可以理解为一个兜底值，当宽度为 auto "
        "时兜底为此值），因此后续要解析的宽度值变为 {}。",
        length, length);
  }
  if (length.HasFitContent()) {
    ipid_depth_log.FPrint("当前的 fit_content_mode: {}",
                          ipid::GetFitContentModeString(fit_content_mode));
  }
  if (length.IsCalculated()) {
    ipid_depth_log.FPrint(
        "当前若 calc 中出现 size 关键字，行为为: {}",
        ipid::GetCalcSizeKeywordBehaviorString(calc_size_keyword_behavior));
  }

  switch (length.GetType()) {
    case Length::kStretch: {
      const LayoutUnit available_size =
          override_available_size == kIndefiniteSize
              ? constraint_space.AvailableSize().inline_size
              : override_available_size;
      ipid_depth_log.FPrint("Length 为 {}，因此首先需获取当前的可用空间。",
                            ipid::GetLengthTypeString(length.GetType()));
      if (override_available_size != kIndefiniteSize) {
        ipid_depth_log.FPrint(
            "上游从从函数参数中传入了 override_available_size: "
            "{}，因此可用空间为该值。",
            override_available_size);
      } else {
        ipid_depth_log.FPrint(
            "可用空间由开辟的 ConstraintSpace 决定，从中取出的可用空间为 {}。",
            available_size);
      }
      if (available_size == kIndefiniteSize) {
        ipid_depth_log.FPrint("可用空间为 -1，因此解析结果也为 -1 (kIndefiniteSize)。return kIndefiniteSize");
        return kIndefiniteSize;
      }
      DCHECK_GE(available_size, LayoutUnit());
      ipid_depth_log.FPrint("正在计算当前节点的 margin 值。");
      const BoxStrut margins = ComputeMarginsForSelf(constraint_space, style);
      const LogicalBoxSides& ignore_margin_sides =
          constraint_space.IgnoreMarginsForStretch();
      return std::max(
          border_padding.InlineSum(),
          available_size -
              (ignore_margin_sides.inline_start ? LayoutUnit()
                                                : margins.inline_start) -
              (ignore_margin_sides.inline_end ? LayoutUnit()
                                              : margins.inline_end));
    }
    case Length::kPercent:
    case Length::kFixed:
    case Length::kCalculated: {
      LayoutUnit percentage_resolution_size =
          constraint_space.PercentageResolutionInlineSize();
      ipid_depth_log.FPrint("当前空间的宽度为 {}。", length,
                            percentage_resolution_size);
      if (length.HasPercent() &&
          percentage_resolution_size == kIndefiniteSize) {
        if (length_type != LengthTypeInternal::kMin) {
          ipid_depth_log.FPrint(
              "宽度的 internal type 为 {}，不是 kMin，因此宽度 {} "
              "的解析结果为 (-1)。",
              ipid::GetLengthTypeInternalString(length_type), length);
          return kIndefiniteSize;
        }
        ipid_depth_log.FPrint(
          "虽然当前空间的宽度为不确定值（-1），但宽度的 internal "
          "type 为 kMin，因此假设当前空间的宽度为 "
          "0（而不是不明确的值），继续接下来的解析逻辑。");
        percentage_resolution_size = LayoutUnit();
      }
      bool evaluated_indefinite = false;

      LayoutUnit value = MinimumValueForLength(
          length, percentage_resolution_size,
          {.intrinsic_evaluator =
               [&](const Length& length_to_evaluate) {
                 IpidDepthLog ipid_depth_log(
                     "length_utils.cc: ResolveInlineLengthInternal 传给 "
                     "MinimumValueForLength 的闭包: intrinsic_evaluator");

                 ipid_depth_log.FPrint(
                     "递归解析宽度：为了最终解析宽度 {}，现在我们递归调用 "
                     "ResolveInlineLengthInternal 来解析宽度 {} 的值。",
                     length, length_to_evaluate);

                 LayoutUnit result = ResolveInlineLengthInternal(
                     constraint_space, style, border_padding,
                     min_max_sizes_func, length_to_evaluate, auto_length,
                     length_type, fit_content_mode, override_available_size,
                     calc_size_keyword_behavior);
                 if (result == kIndefiniteSize) {
                   evaluated_indefinite = true;
                   ipid_depth_log.FPrint("宽度 {} 的解析结果为不明确值 (-1)。",
                                         length);
                   return kIndefiniteSize;
                 }
                 if (style.BoxSizing() == EBoxSizing::kContentBox) {
                   if (border_padding.InlineSum() > LayoutUnit()) {
                     ipid_depth_log.FPrint(
                         "宽度 {} 的解析结果为 {}，但是由于当前元素为 "
                         "box-sizing: content-box，我们需要减去 border + "
                         "padding 的横向值 {}px。",
                         length, result, border_padding.InlineSum());
                   }
                   result -= border_padding.InlineSum();
                 }
                 DCHECK_GE(result, LayoutUnit());

                 ipid_depth_log.FPrint("宽度 {} 的最终解析结果为 {}。", length,
                                       result);
                 return result;
               },
           .calc_size_keyword_behavior = calc_size_keyword_behavior});

      if (evaluated_indefinite) {
        ipid_depth_log.FPrint(
            "上面进行了「递归解析宽度」，其中得到了不明确值 "
            "(-1)，因此宽度 {} 的最终解析结果也为不明确值 (-1)。",
            length);
        return kIndefiniteSize;
      }

      if (style.BoxSizing() == EBoxSizing::kBorderBox) {
        ipid_depth_log.FPrint(
            "当前元素的 box-sizing 为 border-box，因此最终结果为 {} 和 "
            "border+padding 横向值 {} 的较大值。",
            value, border_padding.InlineSum());
        value = std::max(border_padding.InlineSum(), value);
      } else {
        ipid_depth_log.FPrint(
            "当前元素的 box-sizing 为 content-box，因此最终结果为 {} 加上 "
            "border+padding 横向值 {}。",
            value, border_padding.InlineSum());
        value += border_padding.InlineSum();
      }

      ipid_depth_log.FPrint("宽度 {} 的最终解析结果为 {}。", length, value);
      return value;
    }
    case Length::kContent:
    case Length::kMaxContent: {
      ipid_depth_log.FPrint(
          "要解析宽度 {}，就需要算出该元素的固有最大宽度。接下来调用 "
          "min_max_sizes_func(SizeType::kContent) 进行计算。",
          length);
      MinMaxSizes sizes = min_max_sizes_func(SizeType::kContent).sizes;
      LayoutUnit ret = sizes.max_size;
      ipid_depth_log.FPrint(
          "固有宽度计算结果为 {}，取其中的固有最大宽度值 {} 作为最终解析结果。",
          sizes, ret);
      return ret;
    }
    case Length::kMinContent: {
      ipid_depth_log.FPrint(
          "要解析宽度 {}，就需要算出该元素的固有最小宽度。接下来调用 "
          "min_max_sizes_func(SizeType::kContent) 进行计算。",
          length);
      MinMaxSizes sizes = min_max_sizes_func(SizeType::kContent).sizes;
      LayoutUnit ret = sizes.min_size;
      ipid_depth_log.FPrint(
          "固有宽度计算结果为 {}，取其中的固有最小宽度值 {} 作为最终解析结果。",
          ipid::GetMinMaxSizesString(sizes), ret);
      return ret;
    }
    case Length::kMinIntrinsic: {
      ipid_depth_log.FPrint(
          "要解析宽度 {}，就需要算出该元素的固有最小宽度，并且忽略 "
          "aspect-ratio 的影响。接下来调用 "
          "min_max_sizes_func(SizeType::kIntrinsic) 进行计算。",
          length);
      MinMaxSizes sizes = min_max_sizes_func(SizeType::kIntrinsic).sizes;
      LayoutUnit ret = sizes.min_size;
      ipid_depth_log.FPrint(
          "固有宽度（排除 aspect-ratio 影响）计算结果为 "
          "{}，取其中的固有最小宽度值 {} 作为最终解析结果。",
          ipid::GetMinMaxSizesString(sizes), ret);
      return ret;
    }
    case Length::kFitContent: {
      const LayoutUnit available_size =
          override_available_size == kIndefiniteSize
              ? constraint_space.AvailableSize().inline_size
              : override_available_size;

      ipid_depth_log.FPrint(
          "要解析 fit-content "
          "的宽度，则既需要知道元素的固有宽度，又需要知道当前被开辟的空间的宽度"
          "。");
      if (override_available_size == kIndefiniteSize) {
        ipid_depth_log.FPrint(
            "当前被开辟的空间的宽度为 {}，因此可用宽度为该值。",
            available_size);
      } else {
        ipid_depth_log.FPrint(
            "当前函数传入了 override_available_size = {}，因此可用宽度为该值。",
            available_size);
      }

      // fit-content resolves differently depending on the type of length.
      if (available_size == kIndefiniteSize) {
        switch (fit_content_mode) {
          case FitContentMode::kNormal:
            switch (length_type) {
              case LengthTypeInternal::kMin: {
                ipid_depth_log.FPrint(
                    "当前的可用宽度为 -1（不明确值），且当前是在正常地解析 "
                    "fit-content、是从 ResolveMinInlineLength "
                    "中调用本函数。这种情况下，取固有最小宽度作为 fit-content "
                    "的解析结果。");
                MinMaxSizes sizes = min_max_sizes_func(SizeType::kContent).sizes;
                LayoutUnit ret = sizes.min_size;
                ipid_depth_log.FPrint(
                    "固有宽度计算结果为 {}，取其中的固有最小宽度值 {} "
                    "作为最终解析结果。",
                    ipid::GetMinMaxSizesString(sizes), ret);
                return ret;
              }
              case LengthTypeInternal::kMain: {
                ipid_depth_log.FPrint(
                    "当前的可用宽度为 -1（不明确值），且当前是在正常地解析 "
                    "fit-content、是从 ResolveMainInlineLength "
                    "中调用本函数。这种情况下，fit-content "
                    "也只能解析出不明确值 -1。");
                return kIndefiniteSize;
              }
              case LengthTypeInternal::kMax: {
                ipid_depth_log.FPrint(
                    "当前的可用宽度为 -1（不明确值），且当前是在正常地解析 "
                    "fit-content、是从 ResolveMaxInlineLength "
                    "中调用本函数。这种情况下，取固有最大宽度作为 fit-content "
                    "的解析结果。");
                MinMaxSizes sizes = min_max_sizes_func(SizeType::kContent).sizes;
                LayoutUnit ret = sizes.max_size;
                ipid_depth_log.FPrint(
                    "固有宽度计算结果为 {}，取其中的固有最大宽度值 {} "
                    "作为最终解析结果。",
                    ipid::GetMinMaxSizesString(sizes), ret);
                return ret;
              }
            }
          case FitContentMode::kMinContribution: {
            ipid_depth_log.FPrint(
                "当前的可用宽度为 "
                "-1（不明确值），且当前是在计算一个元素对其父元素的固有最小宽度"
                "的贡献值。这种情况下，取固有最小宽度作为 fit-content "
                "的解析结果。");
            MinMaxSizes sizes = min_max_sizes_func(SizeType::kContent).sizes;
            LayoutUnit ret = sizes.min_size;
            ipid_depth_log.FPrint(
                "固有宽度计算结果为 {}，取其中的固有最小宽度值 {} "
                "作为最终解析结果。",
                ipid::GetMinMaxSizesString(sizes), ret);
            return ret;
          }
          case FitContentMode::kMaxContribution: {
            ipid_depth_log.FPrint(
                "当前的可用宽度为 "
                "-1（不明确值），且当前是在计算一个元素对其父元素的固有最大宽度"
                "的贡献值。这种情况下，取固有最大宽度作为 fit-content "
                "的解析结果。");
            MinMaxSizes sizes = min_max_sizes_func(SizeType::kContent).sizes;
            LayoutUnit ret = sizes.max_size;
            ipid_depth_log.FPrint(
                "固有宽度计算结果为 {}，取其中的固有最大宽度值 {} "
                "作为最终解析结果。",
                ipid::GetMinMaxSizesString(sizes), ret);
            return ret;
          }
        }
      }
      DCHECK_GE(available_size, LayoutUnit());

      ipid_depth_log.FPrint("正在计算当前节点的 margin 值。");
      const BoxStrut margins = ComputeMarginsForSelf(constraint_space, style);
      ipid_depth_log.FPrint("计算得到的值为 margin: {}。",
                            ipid::GetBoxStrutString(margins));

      ipid_depth_log.FPrint(
          "当前的可用空间（{}"
          "px）是明确的，因此只要再计算元素的固有宽度，即可获取 fit-content "
          "的解析结果。\n接下来计算元素的固有宽度。",
          available_size);
      MinMaxSizes min_max_func_returned =
          min_max_sizes_func(SizeType::kContent).sizes;
      ipid_depth_log.FPrint(
          "元素固有宽度的计算结果为 {}，我们需要从中减掉 margin 的横向值 "
          "{}px，若减掉后小于 0 就按 0 算。",
          ipid::GetMinMaxSizesString(min_max_func_returned),
          margins.InlineSum());
      LayoutUnit actual_available_size =
          (available_size - margins.InlineSum()).ClampNegativeToZero();
      LayoutUnit ret = min_max_func_returned.ShrinkToFit(actual_available_size);
      ipid_depth_log.FPrint(
          "减掉 margin 后，实际可用空间为 {}。\n然后，执行 fit-content 的 "
          "ShrinkToFit 逻辑，将元素的固有宽度 {} ShrinkToFit 到实际可用空间 {} "
          "上。",
          actual_available_size,
          ipid::GetMinMaxSizesString(min_max_func_returned),
          actual_available_size);

      ipid_depth_log.FPrint(
          "ShrinkToFit 的计算结果为 {}，故 fit-content 的最终解析结果为 {}。",
          ret, ret);
      return ret;
    }
    case Length::kAuto:
      if (length_type == LengthTypeInternal::kMin &&
          RuntimeEnabledFeatures::LayoutMinSizeAutoIndefiniteEnabled()) {
        LayoutUnit ret = border_padding.InlineSum();
        ipid_depth_log.FPrint(
            "[新逻辑 LayoutMinSizeAutoIndefinite 开启] 当前是从 "
            "ResolveMinInlineLength 中调用本函数，因此当解析 auto 时，其值为 "
            "border+padding 的横向值 {}px。",
            ret);
        return ret;
      }
      [[fallthrough]];
    case Length::kNone:
      ipid_depth_log.FPrint("由于直接对 auto 或 none 的宽度作解析，因此解析结果为不明确值 (-1)。");
      return kIndefiniteSize;
    case Length::kFlex:
      NOTREACHED() << "Should only be used for grid.";
    case Length::kDeviceWidth:
    case Length::kDeviceHeight:
    case Length::kExtendToZoom:
      NOTREACHED() << "Should only be used for viewport definitions.";
  }
}

LayoutUnit ResolveBlockLengthInternal(
    const ConstraintSpace& constraint_space,
    const ComputedStyle& style,
    const BoxStrut& border_padding,
    const Length& original_length,
    const Length* auto_length,
    LengthTypeInternal length_type,
    LayoutUnit override_available_size,
    const LayoutUnit* override_percentage_resolution_size,
    BlockSizeFunctionRef block_size_func) {
  IpidDepthLog ipid_depth_log("length_utils.cc: ResolveBlockLengthInternal");

  DCHECK_EQ(constraint_space.GetWritingMode(), style.GetWritingMode());

  ipid_depth_log.FPrint(
      "正在解析高度 {}，\n开辟的空间：{}\n上游需要解析哪种高度：{}\n当前元素的 "
      "border+padding：{}",
      original_length, ipid::GetConstraintSpaceString(constraint_space),
      ipid::GetLengthTypeInternalString(length_type),
      ipid::GetBoxStrutString(border_padding));

  // For min-block-size, this might still be 'auto'.
  const Length& length =
      original_length.IsAuto() && auto_length ? *auto_length : original_length;

  if (original_length.IsAuto() && length.IsAuto()) {
    ipid_depth_log.FPrint(
        "当前要解析的高度为 auto，但上游没有传 "
        "auto_length（可以理解为一个兜底值，当高度为 auto "
        "时兜底为此值），因此后续将直接对 "
        "auto 值作解析。");
  } else if (original_length.IsAuto() && !length.IsAuto()) {
    ipid_depth_log.FPrint(
        "当前要解析的高度为 auto，但上游传入了 auto_length = "
        "{}（可以理解为一个兜底值，当高度为 auto "
        "时兜底为此值），因此后续要解析的高度值变为 {}。",
        length, length);
  }
  switch (length.GetType()) {
    case Length::kStretch: {
      const LayoutUnit available_size =
          override_available_size == kIndefiniteSize
              ? constraint_space.AvailableSize().block_size
              : override_available_size;
      ipid_depth_log.FPrint("Length 为 {}，因此首先需获取当前的可用空间。",
                            ipid::GetLengthTypeString(length.GetType()));
      if (override_available_size != kIndefiniteSize) {
        ipid_depth_log.FPrint(
            "上游从函数参数中传入了 override_available_size: "
            "{}，因此可用空间为该值。",
            override_available_size);
      } else {
        ipid_depth_log.FPrint(
            "可用空间由开辟的 ConstraintSpace 决定，从中取出的可用空间为 {}。",
            available_size);
      }
      if (available_size == kIndefiniteSize) {
        ipid_depth_log.FPrint("可用空间为 -1，需要根据上游调用场景判断解析结果。");
        if (length_type == LengthTypeInternal::kMain) {
          ipid_depth_log.FPrint(
              "上游调用场景为解析主高度，因此调用 "
              "block_size_func(SizeType::kContent) 获取固有高度作为解析结果。");
          LayoutUnit ret = block_size_func(SizeType::kContent);
          ipid_depth_log.FPrint("高度 {} 的最终解析结果为 {}。", length, ret);
          return ret;
        } else {
          ipid_depth_log.FPrint(
              "上游调用场景为解析最小或最大高度，因此解析结果为不明确值 "
              "(-1)。return kIndefiniteSize");
          return kIndefiniteSize;
        }
      }
      DCHECK_GE(available_size, LayoutUnit());
      ipid_depth_log.FPrint("正在计算当前节点的 margin 值。");
      const BoxStrut margins = ComputeMarginsForSelf(constraint_space, style);
      const LogicalBoxSides& ignore_margin_sides =
          constraint_space.IgnoreMarginsForStretch();
      return std::max(
          border_padding.BlockSum(),
          available_size -
              (ignore_margin_sides.block_start ? LayoutUnit()
                                               : margins.block_start) -
              (ignore_margin_sides.block_end ? LayoutUnit()
                                             : margins.block_end));
    }
    case Length::kPercent:
    case Length::kFixed:
    case Length::kCalculated: {
      LayoutUnit percentage_resolution_size =
          override_percentage_resolution_size
              ? *override_percentage_resolution_size
              : constraint_space.PercentageResolutionBlockSize();
      if (override_percentage_resolution_size) {
        ipid_depth_log.FPrint(
            "上游传入了 override_percentage_resolution_size: "
            "{}，用作百分比解析的基准高度。",
            *override_percentage_resolution_size);
      } else {
        ipid_depth_log.FPrint(
            "百分比解析的基准高度由 ConstraintSpace "
            "决定，从中取出的基准高度为 {}。",
            percentage_resolution_size);
      }
      if (length.HasPercent() &&
          percentage_resolution_size == kIndefiniteSize) {
        ipid_depth_log.FPrint(
            "高度包含百分比值，但百分比解析基准为 "
            "-1（不明确），需要根据上游调用场景判断解析结果。");
        switch (length_type) {
          case LengthTypeInternal::kMin: {
            ipid_depth_log.FPrint(
                "上游调用场景为解析最小高度，因此假设百分比解析基准为 "
                "0（而不是不明确的值），继续接下来的解析逻辑。");
            percentage_resolution_size = LayoutUnit();
            break;
          }
          case LengthTypeInternal::kMain: {
            ipid_depth_log.FPrint(
                "上游调用场景为解析主高度，因此调用 "
                "block_size_func(SizeType::kContent) 获取固有高度作为解析结果。");
            LayoutUnit ret = block_size_func(SizeType::kContent);
            ipid_depth_log.FPrint("高度 {} 的最终解析结果为 {}。", length, ret);
            return ret;
          }
          case LengthTypeInternal::kMax: {
            ipid_depth_log.FPrint(
                "上游调用场景为解析最大高度，因此解析结果为不明确值 (-1)。");
            return kIndefiniteSize;
          }
        }
      }
      bool evaluated_indefinite = false;
      LayoutUnit value = MinimumValueForLength(
          length, percentage_resolution_size,
          {.intrinsic_evaluator = [&](const Length& length_to_evaluate) {
            IpidDepthLog ipid_depth_log(
                "length_utils.cc: ResolveBlockLengthInternal 传给 "
                "MinimumValueForLength 的闭包: intrinsic_evaluator");

            ipid_depth_log.FPrint(
                "递归解析高度：为了最终解析高度 {}，现在我们递归调用 "
                "ResolveBlockLengthInternal 来解析高度 {} 的值。",
                length, length_to_evaluate);

            LayoutUnit result = ResolveBlockLengthInternal(
                constraint_space, style, border_padding, length_to_evaluate,
                auto_length, length_type, override_available_size,
                override_percentage_resolution_size, block_size_func);
            if (result == kIndefiniteSize) {
              evaluated_indefinite = true;
              ipid_depth_log.FPrint("高度 {} 的解析结果为不明确值 (-1)。",
                                    length_to_evaluate);
              return kIndefiniteSize;
            }
            if (style.BoxSizing() == EBoxSizing::kContentBox) {
              if (border_padding.BlockSum() > LayoutUnit()) {
                ipid_depth_log.FPrint(
                    "高度 {} 的解析结果为 {}，但是由于当前元素为 "
                    "box-sizing: content-box，我们需要减去 border + "
                    "padding 的纵向值 {}px。",
                    length_to_evaluate, result, border_padding.BlockSum());
              }
              result -= border_padding.BlockSum();
            }
            DCHECK_GE(result, LayoutUnit());

            ipid_depth_log.FPrint("高度 {} 的最终解析结果为 {}。",
                                  length_to_evaluate, result);
            return result;
          }});

      if (evaluated_indefinite) {
        ipid_depth_log.FPrint(
            "上面进行了「递归解析高度」，其中得到了不明确值 "
            "(-1)，因此高度 {} 的最终解析结果也为不明确值 (-1)。",
            length);
        return kIndefiniteSize;
      }

      if (style.BoxSizing() == EBoxSizing::kBorderBox) {
        ipid_depth_log.FPrint(
            "当前元素的 box-sizing 为 border-box，因此最终结果为 {} 和 "
            "border+padding 纵向值 {} 的较大值。",
            value, border_padding.BlockSum());
        value = std::max(border_padding.BlockSum(), value);
      } else {
        ipid_depth_log.FPrint(
            "当前元素的 box-sizing 为 content-box，因此最终结果为 {} 加上 "
            "border+padding 纵向值 {}。",
            value, border_padding.BlockSum());
        value += border_padding.BlockSum();
      }

      ipid_depth_log.FPrint("高度 {} 的最终解析结果为 {}。", length, value);
      return value;
    }
    case Length::kContent:
    case Length::kMinContent:
    case Length::kMaxContent:
    case Length::kMinIntrinsic:
    case Length::kFitContent: {
      ipid_depth_log.FPrint(
          "要解析高度 {}，就需要算出该元素的固有高度。接下来调用 "
          "block_size_func({}) 进行计算。",
          length,
          length.IsMinIntrinsic() ? "SizeType::kIntrinsic" : "SizeType::kContent");
      const LayoutUnit intrinsic_size = block_size_func(
          length.IsMinIntrinsic() ? SizeType::kIntrinsic : SizeType::kContent);
#if DCHECK_IS_ON()
      // Due to how intrinsic_size is calculated, it should always include
      // border and padding. We cannot check for this if we are
      // block-fragmented, though, because then the block-start border/padding
      // may be in a different fragmentainer than the block-end border/padding.
      if (intrinsic_size != kIndefiniteSize &&
          !constraint_space.HasBlockFragmentation())
        DCHECK_GE(intrinsic_size, border_padding.BlockSum());
#endif  // DCHECK_IS_ON()
      ipid_depth_log.FPrint("高度 {} 的最终解析结果为 {}。", length,
                            intrinsic_size);
      return intrinsic_size;
    }
    case Length::kAuto:
      if (length_type == LengthTypeInternal::kMin &&
          RuntimeEnabledFeatures::LayoutMinSizeAutoIndefiniteEnabled()) {
        LayoutUnit ret = border_padding.BlockSum();
        ipid_depth_log.FPrint(
            "[新逻辑 LayoutMinSizeAutoIndefinite 开启] 当前是从 "
            "ResolveMinBlockLength 中调用本函数，因此当解析 auto 时，其值为 "
            "border+padding 的纵向值 {}px。",
            ret);
        return ret;
      }
      [[fallthrough]];
    case Length::kNone:
      ipid_depth_log.FPrint("由于直接对 auto 或 none 的高度作解析，因此解析结果为不明确值 (-1)。");
      return kIndefiniteSize;
    case Length::kFlex:
      NOTREACHED() << "Should only be used for grid.";
    case Length::kDeviceWidth:
    case Length::kDeviceHeight:
    case Length::kExtendToZoom:
      NOTREACHED() << "Should only be used for viewport definitions.";
  }
}

LayoutUnit InlineSizeFromAspectRatio(const BoxStrut& border_padding,
                                     const LogicalSize& aspect_ratio,
                                     EBoxSizing box_sizing,
                                     LayoutUnit block_size) {
  // 以下为旁路日志，完全不侵入 Chromium/Blink
  // 现有代码；但是需要后续 Blink 更新时保证逻辑一致
  IpidDepthLog ipid_depth_log("length_utils.cc: InlineSizeFromAspectRatio");
  LayoutUnit ipidBorderPaddingInline = border_padding.InlineSum();
  ipid_depth_log.FPrint(
      "正在从 aspect-ratio 中获取宽度值。\n\n当前元素 aspect-ratio 为 "
      "{}\nborder+padding 为 {}\n高度为 {}。",
      ipid::GetLogicalSizeString(aspect_ratio),
      ipid::GetBoxStrutString(border_padding), block_size);
  if (box_sizing == EBoxSizing::kBorderBox) {
    LayoutUnit ipidRet =
        block_size.MulDiv(aspect_ratio.inline_size, aspect_ratio.block_size);

    ipid_depth_log.FPrint(
        "当前元素的 box-sizing 为 border-box，因此宽度值 = 高度 {} * (宽 {} / "
        "高 {}) = {}",
        block_size, aspect_ratio.inline_size, aspect_ratio.block_size, ipidRet);

    if (ipidBorderPaddingInline > ipidRet) {
      ipid_depth_log.FPrint(
          "但是由于 border+padding 的横向值： {}px，大于从 aspect-ratio "
          "中得到的宽度值 "
          "{}px，故最终的宽度值计算结果为 {}。",
          ipidBorderPaddingInline, ipidRet, ipidBorderPaddingInline);
    }
  } else {
    LayoutUnit ipidBorderPaddingBlock = border_padding.BlockSum();
    LayoutUnit ipidHeightWithoutBorderPadding =
        block_size - ipidBorderPaddingBlock;
    LayoutUnit ipidRet =
        ipidHeightWithoutBorderPadding.MulDiv(aspect_ratio.inline_size,
                                              aspect_ratio.block_size) +
        ipidBorderPaddingInline;
    ipid_depth_log.FPrint(
        "当前元素的 box-sizing 为 content-box，因此宽度值 = (高度 {} - "
        "border+padding 的纵向值 {}) * (宽 {} / "
        "高 {}) + border+padding 的横向值 {} = {}",
        block_size, ipidBorderPaddingBlock, aspect_ratio.inline_size,
        aspect_ratio.block_size, ipidBorderPaddingInline, ipidRet);
  }

  if (box_sizing == EBoxSizing::kBorderBox) {
    return std::max(
        border_padding.InlineSum(),
        block_size.MulDiv(aspect_ratio.inline_size, aspect_ratio.block_size));
  }
  block_size -= border_padding.BlockSum();
  return block_size.MulDiv(aspect_ratio.inline_size, aspect_ratio.block_size) +
         border_padding.InlineSum();
}

LayoutUnit BlockSizeFromAspectRatio(const BoxStrut& border_padding,
                                    const LogicalSize& aspect_ratio,
                                    EBoxSizing box_sizing,
                                    LayoutUnit inline_size) {
  // 以下为旁路日志，完全不侵入 Chromium/Blink
  // 现有代码；但是需要后续 Blink 更新时保证逻辑一致
  IpidDepthLog ipid_depth_log("length_utils.cc: BlockSizeFromAspectRatio");
  ipid_depth_log.FPrint(
      "正在从 aspect-ratio 中获取高度值。\n\n当前元素 aspect-ratio 为 "
      "{}\nborder+padding 为 {}\n宽度为 {}。",
      ipid::GetLogicalSizeString(aspect_ratio),
      ipid::GetBoxStrutString(border_padding), inline_size);
  if (box_sizing == EBoxSizing::kBorderBox) {
    LayoutUnit ipidRet =
        inline_size.MulDiv(aspect_ratio.block_size, aspect_ratio.inline_size);
    LayoutUnit ipidBorderPaddingBlock = border_padding.BlockSum();

    ipid_depth_log.FPrint(
        "当前元素的 box-sizing 为 border-box，因此高度值 = 宽度 {} * (高 {} / "
        "宽 {}) = {}",
        inline_size, aspect_ratio.block_size, aspect_ratio.inline_size,
        ipidRet);

    if (ipidBorderPaddingBlock > ipidRet) {
      ipid_depth_log.FPrint(
          "但是由于 border+padding 的纵向值： {}px，大于从 aspect-ratio "
          "中得到的高度值 {}px，故最终的高度值计算结果为 {}。",
          ipidBorderPaddingBlock, ipidRet, ipidBorderPaddingBlock);
    }
  } else {
    LayoutUnit ipidBorderPaddingInline = border_padding.InlineSum();
    LayoutUnit ipidWidthWithoutBorderPadding =
        inline_size - ipidBorderPaddingInline;
    LayoutUnit ipidRet =
        ipidWidthWithoutBorderPadding.MulDiv(aspect_ratio.block_size,
                                             aspect_ratio.inline_size) +
        border_padding.BlockSum();
    ipid_depth_log.FPrint(
        "当前元素的 box-sizing 为 content-box，因此高度值 = (宽度 {} - "
        "border+padding 的横向值 {}) * (高 {} / "
        "宽 {}) + border+padding 的纵向值 {} = {}",
        inline_size, ipidBorderPaddingInline, aspect_ratio.block_size,
        aspect_ratio.inline_size, border_padding.BlockSum(), ipidRet);
  }

  DCHECK_GE(inline_size, border_padding.InlineSum());
  if (box_sizing == EBoxSizing::kBorderBox) {
    return std::max(
        border_padding.BlockSum(),
        inline_size.MulDiv(aspect_ratio.block_size, aspect_ratio.inline_size));
  }
  inline_size -= border_padding.InlineSum();
  return inline_size.MulDiv(aspect_ratio.block_size, aspect_ratio.inline_size) +
         border_padding.BlockSum();
}

namespace {

// Currently this simply sets the correct override sizes for the replaced
// element, and lets legacy layout do the result.
MinMaxSizesResult ComputeMinAndMaxContentContributionForReplaced(
    const BlockNode& child,
    const ConstraintSpace& space) {
  const auto& child_style = child.Style();
  const BoxStrut border_padding =
      ComputeBorders(space, child) + ComputePadding(space, child_style);

  MinMaxSizes result;
  result = ComputeReplacedSize(child, space, border_padding).inline_size;

  if (child_style.LogicalWidth().HasPercent() ||
      child_style.LogicalMaxWidth().HasPercent()) {
    // TODO(ikilpatrick): No browser does this today, but we'd get slightly
    // better results here if we also considered the min-block size, and
    // transferred through the aspect-ratio (if available).
    result.min_size = ResolveMinInlineLength(
        space, child_style, border_padding,
        [&](SizeType) -> MinMaxSizesResult {
          // Behave the same as if we couldn't resolve the min-inline size.
          MinMaxSizes sizes;
          sizes = border_padding.InlineSum();
          return {sizes, /* depends_on_block_constraints */ false};
        },
        child_style.LogicalMinWidth());
  }

  // Replaced elements which have a percentage block-size always depend on
  // their block constraints (as they have an aspect-ratio which changes their
  // min/max content size).
  // TODO(https://crbug.com/40339056): These should also check for 'stretch'
  // values.  (We could add Length::MayHaveStretchOrPercentDependence or
  // similar.)
  const bool depends_on_block_constraints =
      child_style.LogicalHeight().MayHavePercentDependence() ||
      child_style.LogicalMinHeight().MayHavePercentDependence() ||
      child_style.LogicalMaxHeight().MayHavePercentDependence() ||
      (child_style.LogicalHeight().HasAuto() &&
       space.IsBlockAutoBehaviorStretch());
  return MinMaxSizesResult(result, depends_on_block_constraints);
}

}  // namespace

MinMaxSizesResult ComputeMinAndMaxContentContributionInternal(
    WritingMode parent_writing_mode,
    const BlockNode& child,
    const ConstraintSpace& space,
    MinMaxSizesFunctionRef original_min_max_sizes_func) {
  IpidDepthLog ipid_depth_log(
      "length_utils.cc: ComputeMinAndMaxContentContributionInternal");

  const auto& style = child.Style();
  const auto border_padding =
      ComputeBorders(space, child) + ComputePadding(space, style);

  ipid_depth_log.FPrint(
      "开始计算元素 {} 对父元素的固有宽度的贡献值。\n"
      "当前元素 border + padding：{}\n"
      "开辟的空间：{}",
      ipid::GetNodeStr(child), ipid::GetBoxStrutString(border_padding),
      ipid::GetConstraintSpaceString(space));

  // First check if we are an orthogonal writing-mode root, then attempt to
  // resolve the block-size.
  if (!IsParallelWritingMode(parent_writing_mode, style.GetWritingMode())) {
    // 打日志不考虑 writing-mode 不为常规值的情况
    const LayoutUnit block_size = ComputeBlockSizeForFragment(
        space, child, border_padding, /* intrinsic_size */ kIndefiniteSize,
        /* inline_size */ kIndefiniteSize);

    // If we weren't able to resolve the block-size, or we might have intrinsic
    // constraints, just perform a full layout via the callback.
    if (block_size == kIndefiniteSize ||
        style.LogicalMinHeight().HasContentOrIntrinsic() ||
        style.LogicalMaxHeight().HasContentOrIntrinsic() || child.IsTable()) {
      return original_min_max_sizes_func(SizeType::kContent);
    }

    return {{block_size, block_size}, /* depends_on_block_constraints */ false};
  }

  // Intercept the min/max sizes function so we can access both the
  // `depends_on_block_constraints` and `applied_aspect_ratio` variables.
  bool depends_on_block_constraints = false;
  bool applied_aspect_ratio = false;
  auto min_max_sizes_func = [&](SizeType type) {
    ipid_depth_log.FPrint(
        "为了计算固有宽度贡献值，现在将调用元素 {} 的 ComputeMinMaxSizes({}) "
        "计算其固有宽度。",
        ipid::GetNodeStr(child), ipid::GetSizeTypeString(type));
    const MinMaxSizesResult result = original_min_max_sizes_func(type);
    depends_on_block_constraints |= result.depends_on_block_constraints;
    applied_aspect_ratio |= result.applied_aspect_ratio;
    ipid_depth_log.FPrint("元素 {} 的固有宽度计算结果为：{}",
                          ipid::GetNodeStr(child), result.sizes);
    return result;
  };

  DCHECK_EQ(space.AvailableSize().inline_size, kIndefiniteSize);

  // First attempt to resolve the main-length, if we can't resolve (e.g. a
  // percentage, or similar) it'll return a kIndefiniteSize.
  const Length& main_length = style.LogicalWidth();
  ipid_depth_log.FPrint(
      "该元素的 CSS 样式中，width 为: {}，正在试图解析该 width", main_length);

  const LayoutUnit extent =
      ResolveMainInlineLength(space, style, border_padding, min_max_sizes_func,
                              main_length, &Length::FitContent());

  ipid_depth_log.FPrint("width: {} 的解析结果为 {}", main_length, extent);

  if (extent == kIndefiniteSize) {
    ipid_depth_log.FPrint(
        "由于 width 解析出的是不明确值 "
        "(-1)"
        "，因此还需要计算该元素自身的固有宽度，才能得到其对父元素的固有宽度贡献"
        "值。");
  } else {
    ipid_depth_log.FPrint(
        "width 解析出的结果是明确值 {}，因此目前直接将该值 ({}, {}) "
        "作为固有宽度贡献值",
        extent, extent, extent);
  }

  // If we successfully resolved our main size, just use that as the
  // contribution, otherwise invoke the callback.
  MinMaxSizes sizes = (extent == kIndefiniteSize)
                          ? min_max_sizes_func(SizeType::kContent).sizes
                          : MinMaxSizes{extent, extent};

  ipid_depth_log.FPrint("经过上述计算，目前得到的固有宽度贡献值为: {}", sizes);

  // If we have calc-size() with a sizing-keyword of auto/fit-content/stretch
  // we need to perform an additional step. Treat the sizing-keyword as auto,
  // then resolve auto as both min-content, and max-content.
  if (main_length.IsCalculated() &&
      (main_length.HasAuto() || main_length.HasFitContent() ||
       main_length.HasStretch())) {
    ipid_depth_log.FPrint(
        "特殊逻辑！在该元素的 CSS width 中检测到了 calc-size() "
        "函数，且函数中包含 auto/fit-content/stretch 关键字，"
        "因此需要执行一个额外的步骤：我们将 content/stretch 关键字视为 "
        "auto，然后将 auto 分别兜底为 min-content 和 max-content，来解析两次 "
        "width "
        "值，所得到的值将覆盖上述步骤得到的值，作为最终的固有宽度贡献值（兜底为"
        " min-content 时为固有最小宽度贡献，兜底为 max-content "
        "时为固有最大宽度贡献）。");

    sizes.min_size = ResolveMainInlineLength(
        space, style, border_padding, min_max_sizes_func, main_length,
        /* auto_length */ &Length::MinContent(),
        /* override_available_size */ kIndefiniteSize,
        CalcSizeKeywordBehavior::kAsAuto);
    sizes.max_size = ResolveMainInlineLength(
        space, style, border_padding, min_max_sizes_func, main_length,
        /* auto_length */ &Length::MaxContent(),
        /* override_available_size */ kIndefiniteSize,
        CalcSizeKeywordBehavior::kAsAuto);

    ipid_depth_log.FPrint("经过上述步骤，目前已经将固有宽度贡献值覆盖为：{}",
                          sizes);
  }

  // Check if we should apply the automatic minimum size.
  // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-minimum
  const Length* auto_min_length =
      (!style.IsScrollContainer() && applied_aspect_ratio)
          ? &Length::MinIntrinsic()
          : nullptr;

  ipid_depth_log.FPrint(
      "接下来，我们计算 min-width 和 max-width 对固有宽度贡献值的影响。");

  // If fit-content is present we need to resolve the min/max sizes twice,
  // once assuming its min-content, and max-content. See:
  // https://github.com/w3c/csswg-drafts/issues/10721
  if (style.LogicalMinWidth().HasFitContent() ||
      style.LogicalMaxWidth().HasFitContent()) {
    ipid_depth_log.FPrint(
        "特殊逻辑！检测到 min-width 或 max-width 使用了 fit-content，"
        "需要分别以 FitContentMode::kMinContribution 和 "
        "FitContentMode::kMaxContribution 方式解析两次该元素的 min-width 和 "
        "max-width。");

    const MinMaxSizes min_sizes = ComputeMinMaxInlineSizes(
        space, child, border_padding, auto_min_length, min_max_sizes_func,
        TransferredSizesMode::kNormal, FitContentMode::kMinContribution);
    const MinMaxSizes max_sizes = ComputeMinMaxInlineSizes(
        space, child, border_padding, auto_min_length, min_max_sizes_func,
        TransferredSizesMode::kNormal, FitContentMode::kMaxContribution);

    ipid_depth_log.FPrint(
        "目前的固有宽度贡献值为 {}\n\n"
        "1. 在 FitContentMode::kMinContribution 模式下，解析出的元素的 "
        "(min-width, max-width) 为：{}。我们要将固有最小宽度的贡献值 {} clamp "
        "到这两个值中间。\n"
        "2. 在 FitContentMode::kMaxContribution 模式下，解析出的元素的 "
        "(min-width, max-width) 为：{}。我们要将固有最大宽度的贡献值 {} clamp "
        "到这两个值中间。",
        sizes, min_sizes, sizes.min_size, max_sizes, sizes.max_size);

    sizes.min_size = min_sizes.ClampSizeToMinAndMax(sizes.min_size);
    sizes.max_size = max_sizes.ClampSizeToMinAndMax(sizes.max_size);

    ipid_depth_log.FPrint("执行 clamp 后，最终得到的固有宽度贡献值为：{}",
                          sizes);
  } else {
    ipid_depth_log.FPrint(
        "我们通过调用 ComputeMinMaxInlineSizes 来计算元素的 min-width 和 "
        "max-width。");

    const MinMaxSizes min_max_sizes = ComputeMinMaxInlineSizes(
        space, child, border_padding, auto_min_length, min_max_sizes_func);

    ipid_depth_log.FPrint(
        "min-width/max-width 计算结果为：{}。\n\n我们的固有宽度贡献值 {} "
        "的两个数字都必须 >= min-width {}，也必须 <= max-width {}。",
        min_max_sizes, sizes, min_max_sizes.min_size, min_max_sizes.max_size);

    sizes.Constrain(min_max_sizes.max_size);
    sizes.Encompass(min_max_sizes.min_size);

    ipid_depth_log.FPrint("按照上述限制计算后，得到的固有宽度贡献值为：{}",
                          sizes);
  }

  const MinMaxSizesResult result = {sizes, depends_on_block_constraints};
  ipid_depth_log.FPrint("元素 {} 的最终的固有宽度贡献值为：{}",
                        ipid::GetNodeStr(child), result.sizes);

  return result;
}

MinMaxSizesResult ComputeMinAndMaxContentContribution(
    const ComputedStyle& parent_style,
    const BlockNode& child,
    const ConstraintSpace& space,
    const MinMaxSizesFloatInput float_input) {
  IpidDepthLog ipid_depth_log(
      "length_utils.cc: ComputeMinAndMaxContentContribution");

  const auto& child_style = child.Style();
  const auto parent_writing_mode = parent_style.GetWritingMode();
  const auto child_writing_mode = child_style.GetWritingMode();

  ipid_depth_log.FPrint(
      "正在计算子元素 {} "
      "对父容器宽度的贡献（即该子元素需要的最小和最大内容宽度）。\n"
      "开辟的空间：{}",
      ipid::GetNodeStr(child), ipid::GetConstraintSpaceString(space));

  if (IsParallelWritingMode(parent_writing_mode, child_writing_mode)) {
    ipid_depth_log.FPrint("父子容器书写模式平行，检查是否为替换元素。");
    if (child.IsReplaced()) {
      ipid_depth_log.FPrint(
          "子元素为替换元素（如 img、video），调用专门的替换元素处理函数。");
      return ComputeMinAndMaxContentContributionForReplaced(child, space);
    }
  } else {
    ipid_depth_log.FPrint(
        "父子容器书写模式正交（如父容器水平，子容器垂直），需要特殊处理。");
  }

  ipid_depth_log.FPrint(
      "创建MinMaxSizesFunc回调函数，用于获取子元素的固有尺寸。");
  auto MinMaxSizesFunc = [&](SizeType type) -> MinMaxSizesResult {
    ipid_depth_log.FPrint("MinMaxSizesFunc被调用，尺寸类型：{}",
                          ipid::GetSizeTypeString(type));
    return child.ComputeMinMaxSizes(parent_writing_mode, type, space,
                                    float_input);
  };

  ipid_depth_log.FPrint(
      "调用ComputeMinAndMaxContentContributionInternal完成具体计算。");
  const MinMaxSizesResult result = ComputeMinAndMaxContentContributionInternal(
      parent_writing_mode, child, space, MinMaxSizesFunc);

  ipid_depth_log.FPrint("计算完成，子元素 {} 的内容贡献为：{}",
                        ipid::GetNodeStr(child),
                        ipid::GetMinMaxSizesResultString(result));

  return result;
}

MinMaxSizesResult ComputeMinAndMaxContentContributionForSelf(
    const BlockNode& child,
    const ConstraintSpace& space) {
  DCHECK(child.CreatesNewFormattingContext());

  const ComputedStyle& child_style = child.Style();
  WritingMode writing_mode = child_style.GetWritingMode();

  if (child.IsReplaced())
    return ComputeMinAndMaxContentContributionForReplaced(child, space);

  auto MinMaxSizesFunc = [&](SizeType type) -> MinMaxSizesResult {
    return child.ComputeMinMaxSizes(writing_mode, type, space);
  };

  return ComputeMinAndMaxContentContributionInternal(writing_mode, child, space,
                                                     MinMaxSizesFunc);
}

MinMaxSizesResult ComputeMinAndMaxContentContributionForSelf(
    const BlockNode& child,
    const ConstraintSpace& space,
    MinMaxSizesFunctionRef min_max_sizes_func) {
  DCHECK(child.CreatesNewFormattingContext());

  return child.IsReplaced()
             ? ComputeMinAndMaxContentContributionForReplaced(child, space)
             : ComputeMinAndMaxContentContributionInternal(
                   child.Style().GetWritingMode(), child, space,
                   min_max_sizes_func);
}

MinMaxSizes ComputeMinAndMaxContentContributionForTest(
    WritingMode parent_writing_mode,
    const BlockNode& child,
    const ConstraintSpace& space,
    const MinMaxSizes& min_max_sizes) {
  auto MinMaxSizesFunc = [&](SizeType) -> MinMaxSizesResult {
    return MinMaxSizesResult(min_max_sizes,
                             /* depends_on_block_constraints */ false);
  };
  return ComputeMinAndMaxContentContributionInternal(parent_writing_mode, child,
                                                     space, MinMaxSizesFunc)
      .sizes;
}

LayoutUnit ComputeInlineSizeForFragmentInternal(
    const ConstraintSpace& space,
    const BlockNode& node,
    const BoxStrut& border_padding,
    MinMaxSizesFunctionRef min_max_sizes_func) {
  IpidDepthLog ipid_depth_log(
      "length_utils.cc: ComputeInlineSizeForFragmentInternal");

  const auto& style = node.Style();
  const Length& logical_width = style.LogicalWidth();

  ipid_depth_log.FPrint(
      "正在计算元素 {} 的宽度。\n"
      "当前元素样式中的 width 属性值：{}\n"
      "开辟的空间：{}\n"
      "当前元素的 border + padding：{}",
      ipid::GetNodeStr(node), logical_width,
      ipid::GetConstraintSpaceString(space),
      ipid::GetBoxStrutString(border_padding));

  const bool may_apply_aspect_ratio = ([&]() {
    if (style.AspectRatio().IsAuto()) {
      ipid_depth_log.FPrint(
          "元素没有设置 aspect-ratio（值为 auto），因此不考虑宽高比的影响。");
      return false;
    }

    ipid_depth_log.FPrint(
        "元素设置了 aspect-ratio: {}，正在检查是否可以应用宽高比约束。",
        style.AspectRatio().GetLayoutRatio());

    // Even though an implicit stretch will resolve - we prefer the inline-axis
    // size for this case.
    if (style.LogicalHeight().HasAuto() &&
        space.BlockAutoBehavior() != AutoSizeBehavior::kStretchExplicit) {
      ipid_depth_log.FPrint(
          "元素的 height 为 auto 且当前空间的高度自动行为为 {} "
          "（不是显式拉伸），此时优先使用宽度轴的尺寸，不应用 aspect-ratio。",
          ipid::GetAutoSizeBehaviorString(space.BlockAutoBehavior()));
      return false;
    }

    // If we can resolve our block-size with no intrinsic-size we can use our
    // aspect-ratio.
    bool can_compute_height =
        ComputeBlockSizeForFragment(space, node, border_padding,
                                    /* intrinsic_size */ kIndefiniteSize,
                                    /* inline_size */ kIndefiniteSize) !=
        kIndefiniteSize;

    if (can_compute_height) {
      ipid_depth_log.FPrint(
          "可以在不依赖固有尺寸的情况下计算出元素的高度，因此可以应用 "
          "aspect-ratio。");
    } else {
      ipid_depth_log.FPrint(
          "无法在不依赖固有尺寸的情况下计算出元素的高度，因此不应用 "
          "aspect-ratio。");
    }

    return can_compute_height;
  })();

  const Length& auto_length = ([&]() {
    ipid_depth_log.FPrint(
        "正在确定当 width 为 auto 时应该使用的兜底值（auto_length）。");

    if (space.AvailableSize().inline_size == kIndefiniteSize) {
      ipid_depth_log.FPrint(
          "当前可用宽度为不确定值（-1），因此 auto 兜底为 min-content。");
      return Length::MinContent();
    }
    if (space.InlineAutoBehavior() == AutoSizeBehavior::kStretchExplicit) {
      ipid_depth_log.FPrint(
          "当前空间的宽度自动行为为 kStretchExplicit（显式拉伸），"
          "因此 auto 兜底为 stretch。");
      return Length::Stretch();
    }
    if (may_apply_aspect_ratio) {
      ipid_depth_log.FPrint(
          "由于可以应用 aspect-ratio，auto 兜底为 fit-content，"
          "这样可以基于内容尺寸和宽高比来确定最终宽度。");
      return Length::FitContent();
    }
    if (space.InlineAutoBehavior() == AutoSizeBehavior::kStretchImplicit) {
      ipid_depth_log.FPrint(
          "当前空间的宽度自动行为为 kStretchImplicit（隐式拉伸），"
          "因此 auto 兜底为 stretch。");
      return Length::Stretch();
    }
    DCHECK_EQ(space.InlineAutoBehavior(), AutoSizeBehavior::kFitContent);
    ipid_depth_log.FPrint(
        "当前空间的宽度自动行为为 kFitContent，因此 auto 兜底为 fit-content。");
    return Length::FitContent();
  })();

  // Check if we should apply the automatic minimum size.
  // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-minimum
  bool apply_automatic_min_size = ([&]() {
    ipid_depth_log.FPrint(
        "正在检查是否应该应用自动最小尺寸（automatic minimum size）。");

    if (style.IsScrollContainer()) {
      ipid_depth_log.FPrint("元素是滚动容器，不应用自动最小尺寸。");
      return false;
    }
    if (!may_apply_aspect_ratio) {
      ipid_depth_log.FPrint("由于不应用 aspect-ratio，不需要自动最小尺寸。");
      return false;
    }
    if (logical_width.HasContentOrIntrinsic()) {
      ipid_depth_log.FPrint(
          "元素的 width 属性值 {} 包含内容相关或固有关键字，应用自动最小尺寸。",
          logical_width);
      return true;
    }
    if (logical_width.HasAuto() && auto_length.HasContentOrIntrinsic()) {
      ipid_depth_log.FPrint(
          "元素的 width 为 auto，且兜底值 {} "
          "包含内容相关或固有关键字，应用自动最小尺寸。",
          auto_length);
      return true;
    }
    ipid_depth_log.FPrint("不满足应用自动最小尺寸的条件。");
    return false;
  })();

  ipid_depth_log.FPrint(
      "接下来调用 ResolveMainInlineLength 来解析元素的主要宽度。\n"
      "width 属性值：{}\n"
      "auto 兜底值：{}",
      logical_width, auto_length);

  const LayoutUnit extent =
      ResolveMainInlineLength(space, style, border_padding, min_max_sizes_func,
                              logical_width, &auto_length);

  ipid_depth_log.FPrint("ResolveMainInlineLength 的解析结果为：{}", extent);

  ipid_depth_log.FPrint(
      "最后调用 ComputeMinMaxInlineSizes 计算 min-width 和 max-width 约束，"
      "并将主要宽度 {} clamp 到这些约束范围内。{}",
      extent,
      apply_automatic_min_size
          ? "由于需要应用自动最小尺寸，将使用 min-intrinsic 作为 min-width 。"
          : "不应用自动最小尺寸约束。");

  MinMaxSizes min_max_sizes = ComputeMinMaxInlineSizes(
      space, node, border_padding,
      apply_automatic_min_size ? &Length::MinIntrinsic() : nullptr,
      min_max_sizes_func);

  LayoutUnit final_width = min_max_sizes.ClampSizeToMinAndMax(extent);

  ipid_depth_log.FPrint(
      "min-width/max-width 约束范围：{}\n"
      "将主要宽度 {} clamp 到约束范围后的最终宽度：{}",
      ipid::GetMinMaxSizesString(min_max_sizes), extent, final_width);

  return final_width;
}

LayoutUnit ComputeInlineSizeForFragment(
    const ConstraintSpace& space,
    const BlockNode& node,
    const BoxStrut& border_padding,
    MinMaxSizesFunctionRef min_max_sizes_func) {
  if (space.IsFixedInlineSize() || space.IsAnonymous()) {
    return space.AvailableSize().inline_size;
  }

  if (node.IsTable()) {
    return To<TableNode>(node).ComputeTableInlineSize(space, border_padding);
  }

  return ComputeInlineSizeForFragmentInternal(space, node, border_padding,
                                              min_max_sizes_func);
}

LayoutUnit ComputeUsedInlineSizeForTableFragment(
    const ConstraintSpace& space,
    const BlockNode& node,
    const BoxStrut& border_padding,
    const MinMaxSizes& table_grid_min_max_sizes) {
  DCHECK(!space.IsFixedInlineSize());

  auto MinMaxSizesFunc = [&](SizeType type) -> MinMaxSizesResult {
    const auto& style = node.Style();
    const bool has_aspect_ratio = !style.AspectRatio().IsAuto();

    // Check if we have an aspect-ratio.
    if (has_aspect_ratio && type == SizeType::kContent) {
      const LayoutUnit block_size =
          ComputeBlockSizeForFragment(space, node, border_padding,
                                      /* intrinsic_size */ kIndefiniteSize,
                                      /* inline_size */ kIndefiniteSize);
      if (block_size != kIndefiniteSize) {
        const LayoutUnit inline_size = InlineSizeFromAspectRatio(
            border_padding, style.LogicalAspectRatio(),
            style.BoxSizingForAspectRatio(), block_size);
        return MinMaxSizesResult({inline_size, inline_size},
                                 /* depends_on_block_constraints */ false);
      }
    }
    return MinMaxSizesResult(table_grid_min_max_sizes,
                             /* depends_on_block_constraints */ false);
  };

  return ComputeInlineSizeForFragmentInternal(space, node, border_padding,
                                              MinMaxSizesFunc);
}

MinMaxSizes ComputeInitialMinMaxBlockSizes(const ConstraintSpace& space,
                                           const BlockNode& node,
                                           const BoxStrut& border_padding,
                                           LayoutUnit override_available_size) {
  const ComputedStyle& style = node.Style();
  MinMaxSizes sizes = {ResolveInitialMinBlockLength(
                           space, style, border_padding,
                           style.LogicalMinHeight(), override_available_size),
                       ResolveInitialMaxBlockLength(
                           space, style, border_padding,
                           style.LogicalMaxHeight(), override_available_size)};
  sizes.max_size = std::max(sizes.max_size, sizes.min_size);
  return sizes;
}

MinMaxSizes ComputeMinMaxBlockSizes(const ConstraintSpace& space,
                                    const BlockNode& node,
                                    const BoxStrut& border_padding,
                                    const Length* auto_min_length,
                                    BlockSizeFunctionRef block_size_func,
                                    LayoutUnit override_available_size) {
  IpidDepthLog ipid_depth_log("length_utils.cc: ComputeMinMaxBlockSizes");

  const ComputedStyle& style = node.Style();

  ipid_depth_log.FPrint(
      "开始计算元素 {} 的 min-height 和 max-height 像素值。\n"
      "开辟的空间：{}\n"
      "当前元素的 border+padding：{}\n"
      "CSS min-height 属性值：{}\n"
      "CSS max-height 属性值：{}",
      ipid::GetNodeStr(node), ipid::GetConstraintSpaceString(space),
      ipid::GetBoxStrutString(border_padding), style.LogicalMinHeight(),
      style.LogicalMaxHeight());

  if (auto_min_length) {
    ipid_depth_log.FPrint(
        "检测到自动最小高度参数：{}，这通常用于 aspect-ratio "
        "场景下的自动最小约束。",
        *auto_min_length);
  } else {
    ipid_depth_log.FPrint(
        "未传入自动最小高度参数，将直接使用 CSS min-height 属性。");
  }

  if (override_available_size != kIndefiniteSize) {
    ipid_depth_log.FPrint(
        "上游传入了 override_available_size: "
        "{}，这将覆盖约束空间中的可用高度。",
        override_available_size);
  }

  ipid_depth_log.FPrint(
      "第 1 步：计算 min-height 像素值（调用 ResolveMinBlockLength）");

  LayoutUnit min_size = ResolveMinBlockLength(
      space, style, border_padding, block_size_func, style.LogicalMinHeight(),
      auto_min_length, override_available_size);

  ipid_depth_log.FPrint("第 1 步完成：min-height计算结果为 {}", min_size);

  ipid_depth_log.FPrint(
      "第 2 步：计算 max-height 像素值（调用 ResolveMaxBlockLength）");

  LayoutUnit max_size = ResolveMaxBlockLength(
      space, style, border_padding, style.LogicalMaxHeight(), block_size_func,
      override_available_size);

  ipid_depth_log.FPrint("第 2 步完成：max-height 像素值计算结果为 {}",
                        max_size);

  ipid_depth_log.FPrint(
      "第 3 步：构建初始 MinMaxSizes 对象，当前状态：({}, {})", min_size,
      max_size);

  MinMaxSizes sizes = {min_size, max_size};

  // Clamp the auto min-size by the max-size.
  if (auto_min_length && style.LogicalMinHeight().HasAuto()) {
    ipid_depth_log.FPrint(
        "第 4 步：检测到需要限制自动最小高度。\n"
        "条件满足：传入了 auto_min_length ({}) 且 CSS min-height 为 auto\n"
        "操作前状态：({}, {})\n"
        "将最小高度限制为不超过最大高度",
        *auto_min_length, sizes.min_size, sizes.max_size);

    sizes.min_size = std::min(sizes.min_size, sizes.max_size);

    ipid_depth_log.FPrint("第 4 步完成：限制后的状态：({}, {})", sizes.min_size,
                          sizes.max_size);
  } else {
    ipid_depth_log.FPrint(
        "第 4 步：跳过自动最小高度限制。\n"
        "原因：{}",
        !auto_min_length ? "未传入 auto_min_length"
                         : "CSS min-height 不是 auto");
  }

  // Tables can't shrink below their min-intrinsic size.
  if (node.IsTable()) {
    ipid_depth_log.FPrint(
        "第 5 步：检测到当前元素为表格，需要应用表格特殊约束。\n"
        "根据 CSS 表格规范，表格不能收缩到小于其固有最小尺寸。\n"
        "操作前状态：({}, {})",
        sizes.min_size, sizes.max_size);

    ipid_depth_log.FPrint(
        "正在调用 block_size_func(SizeType::kIntrinsic) 获取表格的固有尺寸。");
    LayoutUnit intrinsic_size = block_size_func(SizeType::kIntrinsic);

    ipid_depth_log.FPrint(
        "表格的固有尺寸为：{}\n"
        "使用 Encompass 方法确保最小尺寸包含固有尺寸",
        intrinsic_size);

    sizes.Encompass(intrinsic_size);

    ipid_depth_log.FPrint("第 5 步完成：应用表格约束后的状态：({}, {})",
                          sizes.min_size, sizes.max_size);
  } else {
    ipid_depth_log.FPrint("第 5 步：跳过表格特殊约束（当前元素不是表格）。");
  }

  ipid_depth_log.FPrint(
      "第 6 步：确保最大高度不小于最小高度（CSS 规范要求）。\n"
      "操作前状态：({}, {})",
      sizes.min_size, sizes.max_size);

  sizes.max_size = std::max(sizes.max_size, sizes.min_size);

  ipid_depth_log.FPrint("第 6 步完成：调整后的最终状态：({}, {})",
                        sizes.min_size, sizes.max_size);

  ipid_depth_log.FPrint(
      "元素 {} 的 min 和 max-height 像素值计算完成，最终结果：{}",
      ipid::GetNodeStr(node), ipid::GetMinMaxSizesString(sizes));

  return sizes;
}

MinMaxSizes ComputeTransferredMinMaxInlineSizes(
    const LogicalSize& ratio,
    const MinMaxSizes& block_min_max,
    const BoxStrut& border_padding,
    const EBoxSizing sizing) {
  IpidDepthLog ipid_depth_log(
      "length_utils.cc: ComputeTransferredMinMaxInlineSizes");
  DCHECK(!ratio.IsEmpty());

  ipid_depth_log.FPrint(
      "通过 aspect-ratio 从高度约束转换为宽度约束。\n"
      "aspect-ratio：{}\n"
      "输入的高度约束：{}\n"
      "border+padding：{}\n"
      "box-sizing：{}",
      ipid::GetAspectRatioString(ratio),
      ipid::GetMinMaxSizesString(block_min_max),
      ipid::GetBoxStrutString(border_padding),
      (sizing == EBoxSizing::kContentBox ? "content-box" : "border-box"));

  MinMaxSizes transferred_min_max = {LayoutUnit(), LayoutUnit::Max()};

  ipid_depth_log.FPrint(
      "步骤1：转换最小高度约束为最小宽度约束。");
  if (block_min_max.min_size > LayoutUnit()) {
    ipid_depth_log.FPrint(
        "最小高度约束为 {}px > 0，调用 InlineSizeFromAspectRatio 进行转换。",
        block_min_max.min_size);
    transferred_min_max.min_size = InlineSizeFromAspectRatio(
        border_padding, ratio, sizing, block_min_max.min_size);
    ipid_depth_log.FPrint("转换后的最小宽度约束：{}px",
                          transferred_min_max.min_size);
  } else {
    ipid_depth_log.FPrint(
        "最小高度约束为 {}px <= 0，最小宽度约束保持默认值 0px。",
        block_min_max.min_size);
  }

  ipid_depth_log.FPrint(
      "步骤2：转换最大高度约束为最大宽度约束。");
  if (block_min_max.max_size != LayoutUnit::Max()) {
    ipid_depth_log.FPrint(
        "最大高度约束为 {}px，不是无限大值，调用 InlineSizeFromAspectRatio 进行转换。",
        block_min_max.max_size);
    transferred_min_max.max_size = InlineSizeFromAspectRatio(
        border_padding, ratio, sizing, block_min_max.max_size);
    ipid_depth_log.FPrint("转换后的最大宽度约束：{}px",
                          transferred_min_max.max_size);
  } else {
    ipid_depth_log.FPrint(
        "最大高度约束为无限大值，最大宽度约束保持默认无限大值。");
  }

  ipid_depth_log.FPrint(
      "步骤3：确保最大宽度约束不小于最小宽度约束。\n"
      "调整前：{}",
      ipid::GetMinMaxSizesString(transferred_min_max));

  // Minimum size wins over maximum size.
  transferred_min_max.max_size =
      std::max(transferred_min_max.max_size, transferred_min_max.min_size);

  ipid_depth_log.FPrint(
      "通过 aspect-ratio 转换得到的最终宽度约束：{}\n"
      "- 最小宽度约束：{}px\n"
      "- 最大宽度约束：{}px",
      ipid::GetMinMaxSizesString(transferred_min_max),
      transferred_min_max.min_size, transferred_min_max.max_size);

  return transferred_min_max;
}

MinMaxSizes ComputeTransferredMinMaxBlockSizes(
    const LogicalSize& ratio,
    const MinMaxSizes& inline_min_max,
    const BoxStrut& border_padding,
    const EBoxSizing sizing) {
  IpidDepthLog ipid_depth_log(
      "length_utils.cc: ComputeTransferredMinMaxBlockSizes");

  ipid_depth_log.FPrint(
      "通过 aspect-ratio 从宽度约束转换为高度约束。\n"
      "aspect-ratio：{}\n"
      "输入的宽度约束：{}\n"
      "border+padding：{}\n"
      "box-sizing：{}",
      ipid::GetAspectRatioString(ratio),
      ipid::GetMinMaxSizesString(inline_min_max),
      ipid::GetBoxStrutString(border_padding),
      (sizing == EBoxSizing::kContentBox ? "content-box" : "border-box"));

  MinMaxSizes transferred_min_max = {LayoutUnit(), LayoutUnit::Max()};

  ipid_depth_log.FPrint(
      "步骤1：转换最小宽度约束为最小高度约束。");
  if (inline_min_max.min_size > LayoutUnit()) {
    ipid_depth_log.FPrint(
        "最小宽度约束为 {}px > 0，调用 BlockSizeFromAspectRatio 进行转换。",
        inline_min_max.min_size);
    transferred_min_max.min_size = BlockSizeFromAspectRatio(
        border_padding, ratio, sizing, inline_min_max.min_size);
    ipid_depth_log.FPrint("转换后的最小高度约束：{}px",
                          transferred_min_max.min_size);
  } else {
    ipid_depth_log.FPrint(
        "最小宽度约束为 {}px <= 0，最小高度约束保持默认值 0px。",
        inline_min_max.min_size);
  }

  ipid_depth_log.FPrint(
      "步骤2：转换最大宽度约束为最大高度约束。");
  if (inline_min_max.max_size != LayoutUnit::Max()) {
    ipid_depth_log.FPrint(
        "最大宽度约束为 {}px，不是无限大值，调用 BlockSizeFromAspectRatio 进行转换。",
        inline_min_max.max_size);
    transferred_min_max.max_size = BlockSizeFromAspectRatio(
        border_padding, ratio, sizing, inline_min_max.max_size);
    ipid_depth_log.FPrint("转换后的最大高度约束：{}px",
                          transferred_min_max.max_size);
  } else {
    ipid_depth_log.FPrint(
        "最大宽度约束为无限大值，最大高度约束保持默认无限大值。");
  }

  ipid_depth_log.FPrint(
      "步骤3：确保最大高度约束不小于最小高度约束。\n"
      "调整前：{}",
      ipid::GetMinMaxSizesString(transferred_min_max));

  // Minimum size wins over maximum size.
  transferred_min_max.max_size =
      std::max(transferred_min_max.max_size, transferred_min_max.min_size);

  ipid_depth_log.FPrint(
      "通过 aspect-ratio 转换得到的最终高度约束：{}\n"
      "- 最小高度约束：{}px\n"
      "- 最大高度约束：{}px",
      ipid::GetMinMaxSizesString(transferred_min_max),
      transferred_min_max.min_size, transferred_min_max.max_size);

  return transferred_min_max;
}

MinMaxSizes ComputeMinMaxInlineSizesFromAspectRatio(
    const ConstraintSpace& constraint_space,
    const BlockNode& node,
    const BoxStrut& border_padding) {
  IpidDepthLog ipid_depth_log(
      "length_utils.cc: ComputeMinMaxInlineSizesFromAspectRatio");

  // The spec requires us to clamp these by the specified size (it calls it the
  // preferred size). However, we actually don't need to worry about that,
  // because we only use this if the width is indefinite.

  // We do not need to compute the min/max inline sizes; as long as we always
  // apply the transferred min/max size before the explicit min/max size, the
  // result will be identical.
  const ComputedStyle& style = node.Style();
  DCHECK(!style.AspectRatio().IsAuto());

  ipid_depth_log.FPrint(
      "通过 aspect-ratio 计算元素 {} 的宽度约束。\n"
      "开辟的空间：{}\n"
      "元素 border+padding：{}\n"
      "aspect-ratio：{}\n"
      "box-sizing：{}",
      ipid::GetNodeStr(node), ipid::GetConstraintSpaceString(constraint_space),
      ipid::GetBoxStrutString(border_padding),
      ipid::GetAspectRatioString(style.LogicalAspectRatio()),
      (style.BoxSizingForAspectRatio() == EBoxSizing::kContentBox
           ? "content-box"
           : "border-box"));

  ipid_depth_log.FPrint(
      "步骤1：调用 ComputeInitialMinMaxBlockSizes 计算元素的高度约束。");
  const MinMaxSizes block_min_max =
      ComputeInitialMinMaxBlockSizes(constraint_space, node, border_padding);
  ipid_depth_log.FPrint("计算得到的高度约束：{}", 
                        ipid::GetMinMaxSizesString(block_min_max));

  ipid_depth_log.FPrint(
      "步骤2：调用 ComputeTransferredMinMaxInlineSizes 通过 aspect-ratio "
      "从高度约束转换为宽度约束。");
  MinMaxSizes result = ComputeTransferredMinMaxInlineSizes(
      style.LogicalAspectRatio(), block_min_max, border_padding,
      style.BoxSizingForAspectRatio());

  ipid_depth_log.FPrint(
      "从 aspect-ratio 计算得到的最终宽度约束：{}\n"
      "- 最小宽度约束：{}px\n"
      "- 最大宽度约束：{}px",
      ipid::GetMinMaxSizesString(result), result.min_size, result.max_size);

  return result;
}

MinMaxSizes ComputeMinMaxInlineSizes(
    const ConstraintSpace& space,
    const BlockNode& node,
    const BoxStrut& border_padding,
    const Length* auto_min_length,
    MinMaxSizesFunctionRef min_max_sizes_func,
    TransferredSizesMode transferred_sizes_mode,
    FitContentMode fit_content_mode,
    LayoutUnit override_available_size) {
  IpidDepthLog ipid_depth_log("length_utils.cc: ComputeMinMaxInlineSizes");

  const ComputedStyle& style = node.Style();

  ipid_depth_log.FPrint(
      "正在计算元素 {} 的宽度约束 (min-width/max-width)。\n"
      "开辟的空间：{}\n"
      "元素 border+padding 值：{}\n"
      "CSS min-width 属性：{}\n"
      "CSS max-width 属性：{}\n"
      "transferred_sizes_mode：{}\n"
      "fit_content_mode：{}",
      ipid::GetNodeStr(node), ipid::GetConstraintSpaceString(space),
      ipid::GetBoxStrutString(border_padding), style.LogicalMinWidth(),
      style.LogicalMaxWidth(),
      (transferred_sizes_mode == TransferredSizesMode::kNormal ? "kNormal"
                                                               : "kIgnore"),
      ipid::GetFitContentModeString(fit_content_mode));

  if (auto_min_length) {
    ipid_depth_log.FPrint("若 min-width 为 auto，兜底为此值：{}",
                          *auto_min_length);
  }

  ipid_depth_log.FPrint(
      "步骤1：解析 CSS min-width 和 max-width 属性为像素值。");

  MinMaxSizes sizes = {
      ResolveMinInlineLength(space, style, border_padding, min_max_sizes_func,
                             style.LogicalMinWidth(), auto_min_length,
                             override_available_size, fit_content_mode),
      ResolveMaxInlineLength(space, style, border_padding, min_max_sizes_func,
                             style.LogicalMaxWidth(), override_available_size,
                             fit_content_mode)};

  ipid_depth_log.FPrint(
      "CSS min-width 解析结果：{}px\n"
      "CSS max-width 解析结果：{}px\n"
      "初始的宽度约束为：{}",
      sizes.min_size, sizes.max_size, ipid::GetMinMaxSizesString(sizes));

  // Clamp the auto min-size by the max-size.
  if (auto_min_length && style.LogicalMinWidth().HasAuto()) {
    ipid_depth_log.FPrint(
        "步骤2：min-width 为 auto 且传入了 auto_min_length 兜底值，需要将 "
        "min-width 限制为不超过 max-width 。\n"
        "限制前：min-width = {}px\n"
        "max-width = {}px",
        sizes.min_size, sizes.max_size);
    sizes.min_size = std::min(sizes.min_size, sizes.max_size);
    ipid_depth_log.FPrint("限制后：min-width = {}px", sizes.min_size);
  } else {
    ipid_depth_log.FPrint(
        "步骤2：跳过 auto min-width 限制（min-width 不为 auto "
        "或未传入兜底值）。");
  }

  // This implements the transferred min/max sizes per:
  // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-size-transfers
  if (transferred_sizes_mode == TransferredSizesMode::kNormal &&
      !style.AspectRatio().IsAuto() && style.LogicalWidth().HasAuto() &&
      space.InlineAutoBehavior() != AutoSizeBehavior::kStretchExplicit) {
    ipid_depth_log.FPrint(
        "步骤3：检测到满足 aspect-ratio 宽度约束转换条件：\n"
        "- transferred_sizes_mode = kNormal\n"
        "- aspect-ratio = {}\n"
        "- CSS width 为 auto\n"
        "- InlineAutoBehavior != kStretchExplicit\n"
        "正在调用 ComputeMinMaxInlineSizesFromAspectRatio 计算从 aspect-ratio "
        "转换而来的宽度约束。",
        ipid::GetAspectRatioString(style.LogicalAspectRatio()));

    MinMaxSizes transferred_sizes =
        ComputeMinMaxInlineSizesFromAspectRatio(space, node, border_padding);

    ipid_depth_log.FPrint(
        "从 aspect-ratio 计算得到的宽度约束：{}\n"
        "当前的宽度约束：{}\n"
        "现在将两者合并：\n"
        "- 新的 min-width = max(当前最小宽度, min(转换最小宽度, "
        "当前最大宽度))\n"
        "- 新的 max-width = min(当前最大宽度, 转换最大宽度)",
        ipid::GetMinMaxSizesString(transferred_sizes),
        ipid::GetMinMaxSizesString(sizes));

    LayoutUnit old_min = sizes.min_size;
    LayoutUnit old_max = sizes.max_size;

    sizes.min_size = std::max(
        sizes.min_size, std::min(transferred_sizes.min_size, sizes.max_size));
    sizes.max_size = std::min(sizes.max_size, transferred_sizes.max_size);

    ipid_depth_log.FPrint(
        "合并后的宽度约束：{}\n"
        " min-width 从 {}px 变为 {}px\n"
        " max-width 从 {}px 变为 {}px",
        ipid::GetMinMaxSizesString(sizes), old_min, sizes.min_size, old_max,
        sizes.max_size);
  } else {
    ipid_depth_log.FPrint(
        "步骤3：跳过 aspect-ratio 宽度约束转换（不满足转换条件）。\n"
        "- transferred_sizes_mode = {}\n"
        "- aspect-ratio.IsAuto() = {}\n"
        "- width.HasAuto() = {}\n"
        "- InlineAutoBehavior = {}",
        (transferred_sizes_mode == TransferredSizesMode::kNormal ? "kNormal"
                                                                 : "kIgnore"),
        ipid::btos(style.AspectRatio().IsAuto()),
        ipid::btos(style.LogicalWidth().HasAuto()),
        ipid::GetAutoSizeBehaviorString(space.InlineAutoBehavior()));
  }

  // Tables can't shrink below their min-intrinsic size.
  if (node.IsTable()) {
    ipid_depth_log.FPrint(
        "步骤4：检测到 table 元素，需要确保宽度约束不会小于其最小固有尺寸。");
    LayoutUnit table_min_intrinsic =
        min_max_sizes_func(SizeType::kIntrinsic).sizes.min_size;
    ipid_depth_log.FPrint(
        "table 元素的最小固有尺寸：{}px\n"
        "应用前的宽度约束：{}",
        table_min_intrinsic, ipid::GetMinMaxSizesString(sizes));
    sizes.Encompass(table_min_intrinsic);
    ipid_depth_log.FPrint("应用后的宽度约束：{}",
                          ipid::GetMinMaxSizesString(sizes));
  } else {
    ipid_depth_log.FPrint("步骤4：跳过 table 特殊处理（非 table 元素）。");
  }

  ipid_depth_log.FPrint(
      "步骤5：确保 max-width 不小于 min-width （符合 CSS 规范要求）。\n"
      "调整前：{}",
      ipid::GetMinMaxSizesString(sizes));
  sizes.max_size = std::max(sizes.max_size, sizes.min_size);

  ipid_depth_log.FPrint(
      "最终的宽度约束结果：{}\n"
      "- min-width ：{}px\n"
      "- max-width ：{}px",
      ipid::GetMinMaxSizesString(sizes), sizes.min_size, sizes.max_size);

  return sizes;
}

namespace {

// Computes the block-size for a fragment, ignoring the fixed block-size if set.
LayoutUnit ComputeBlockSizeForFragmentInternal(
    const ConstraintSpace& space,
    const BlockNode& node,
    const BoxStrut& border_padding,
    LayoutUnit intrinsic_size,
    LayoutUnit inline_size,
    LayoutUnit override_available_size = kIndefiniteSize) {
  IpidDepthLog ipid_depth_log(
      "length_utils.cc: ComputeBlockSizeForFragmentInternal");

  const ComputedStyle& style = node.Style();

  ipid_depth_log.FPrint(
      "开始计算元素 {} 的高度，忽略固定高度设置。\n"
      "开辟的空间：{}\n"
      "当前元素的 border+padding：{}\n"
      "固有高度（通常为内容高度）：{}\n"
      "宽度值（用于 aspect-ratio 计算）：{}",
      ipid::GetNodeStr(node), ipid::GetConstraintSpaceString(space),
      ipid::GetBoxStrutString(border_padding), intrinsic_size, inline_size);

  if (override_available_size != kIndefiniteSize) {
    ipid_depth_log.FPrint(
        "上游传入了 override_available_size: "
        "{}，这通常用于特殊布局算法（如表格）。",
        override_available_size);
  }

  // Scrollable percentage-sized children of table cells (sometimes) are sized
  // to their initial min-size.
  // See: https://drafts.csswg.org/css-tables-3/#row-layout
  if (space.IsRestrictedBlockSizeTableCellChild()) {
    ipid_depth_log.FPrint(
        "[特殊情况] 检测到当前元素为表格单元格的子元素，且存在高度约束限制。"
        "根据 CSS 表格规范，这种情况下将使用初始最小高度进行计算。\n"
        "当前元素的 min-height 值为：{}",
        style.LogicalMinHeight());
    LayoutUnit result = ResolveInitialMinBlockLength(
        space, style, border_padding, style.LogicalMinHeight(),
        override_available_size);
    ipid_depth_log.FPrint("表格单元格子元素的高度计算结果：{}", result);
    return result;
  }

  const Length& logical_height = style.LogicalHeight();
  const bool has_aspect_ratio = !style.AspectRatio().IsAuto();
  const bool may_apply_aspect_ratio =
      has_aspect_ratio && inline_size != kIndefiniteSize;

  ipid_depth_log.FPrint(
      "读取元素的 CSS 高度设置：{}\n"
      "检查是否有 aspect-ratio：{}\n"
      "是否可以应用 aspect-ratio：{}",
      logical_height, has_aspect_ratio ? "是" : "否",
      may_apply_aspect_ratio ? "是（有 aspect-ratio 且宽度已确定）" : "否");

  ipid_depth_log.FPrint(
      "开始确定当高度为 auto 时的兜底行为。\n"
      "当前开辟空间的可用高度：{}\n"
      "当前空间的 BlockAutoBehavior：{}",
      space.AvailableSize().block_size,
      ipid::GetAutoSizeBehaviorString(space.BlockAutoBehavior()));

  const Length& auto_length = ([&]() {
    if (space.AvailableSize().block_size == kIndefiniteSize) {
      ipid_depth_log.FPrint(
          "可用高度为不明确值 (-1)，因此 auto 高度兜底为 fit-content。");
      return Length::FitContent();
    }
    if (space.BlockAutoBehavior() == AutoSizeBehavior::kStretchExplicit) {
      ipid_depth_log.FPrint(
          "空间要求明确拉伸到可用高度，因此 auto 高度兜底为 stretch。");
      return Length::Stretch();
    }
    if (may_apply_aspect_ratio) {
      ipid_depth_log.FPrint(
          "由于存在 aspect-ratio 且宽度已确定，为保持比例，auto 高度兜底为 "
          "fit-content。");
      return Length::FitContent();
    }
    if (space.BlockAutoBehavior() == AutoSizeBehavior::kStretchImplicit) {
      ipid_depth_log.FPrint(
          "空间要求隐式拉伸到可用高度，因此 auto 高度兜底为 stretch。");
      return Length::Stretch();
    }
    DCHECK_EQ(space.BlockAutoBehavior(), AutoSizeBehavior::kFitContent);
    ipid_depth_log.FPrint("默认情况下，auto 高度兜底为 fit-content。");
    return Length::FitContent();
  })();

  ipid_depth_log.FPrint("确定的 auto 高度兜底行为：{}", auto_length);

  // Check if we should apply the automatic minimum size.
  // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-minimum
  ipid_depth_log.FPrint(
      "检查是否需要应用 aspect-ratio 的自动min-height。\n"
      "根据 CSS 规范，当元素有 aspect-ratio "
      "且满足特定条件时，需要应用自动最小高度。");

  bool apply_automatic_min_size = ([&]() {
    // We check for LayoutUnit::Max() as flexbox uses this as a "placeholder"
    // to compute the flex line length while still respecting max-block-size.
    if (intrinsic_size == kIndefiniteSize ||
        intrinsic_size == LayoutUnit::Max()) {
      ipid_depth_log.FPrint(
          "固有高度为不明确值或 Max 值（Flexbox "
          "占位符），不应用自动最小高度。");
      return false;
    }
    if (style.IsScrollContainer()) {
      ipid_depth_log.FPrint("当前元素为滚动容器，不应用自动最小高度。");
      return false;
    }
    if (!may_apply_aspect_ratio) {
      ipid_depth_log.FPrint(
          "无法应用 "
          "aspect-ratio（没有比例或宽度不明确），不应用自动最小高度。");
      return false;
    }
    if (logical_height.HasContentOrIntrinsic()) {
      ipid_depth_log.FPrint(
          "CSS 高度值包含 content 或 intrinsic 关键字，应用自动最小高度。");
      return true;
    }
    if (logical_height.HasAuto() && auto_length.HasContentOrIntrinsic()) {
      ipid_depth_log.FPrint(
          "CSS 高度为 auto 且兜底值包含 content 或 "
          "intrinsic，应用自动最小高度。");
      return true;
    }
    ipid_depth_log.FPrint("不满足应用自动最小高度的条件。");
    return false;
  })();

  ipid_depth_log.FPrint("是否应用自动最小高度：{}",
                        apply_automatic_min_size ? "是" : "否");

  ipid_depth_log.FPrint("定义高度计算函数，用于在需要固有尺寸时提供高度值。");

  auto BlockSizeFunc = [&](SizeType type) {
    if (type == SizeType::kContent && has_aspect_ratio &&
        inline_size != kIndefiniteSize) {
      ipid_depth_log.FPrint(
          "需要计算 content 高度且有 aspect-ratio，从 aspect-ratio 计算高度。\n"
          "aspect-ratio：{}\n"
          "宽度：{}",
          ipid::GetLogicalSizeString(style.LogicalAspectRatio()), inline_size);
      LayoutUnit result = BlockSizeFromAspectRatio(
          border_padding, style.LogicalAspectRatio(),
          style.BoxSizingForAspectRatio(), inline_size);
      ipid_depth_log.FPrint("从 aspect-ratio 计算得到的高度：{}", result);
      return result;
    }
    ipid_depth_log.FPrint("使用预设的固有高度：{}", intrinsic_size);
    return intrinsic_size;
  };

  ipid_depth_log.FPrint("开始解析主要高度值（即 CSS height 属性）。");

  const LayoutUnit extent = ResolveMainBlockLength(
      space, style, border_padding, logical_height, &auto_length, BlockSizeFunc,
      override_available_size);

  ipid_depth_log.FPrint("主要高度解析结果：{}", extent);

  if (extent == kIndefiniteSize) {
    DCHECK_EQ(intrinsic_size, kIndefiniteSize);
    ipid_depth_log.FPrint(
        "主要高度为不明确值 (-1)，无法计算确定的高度，直接返回不明确值。");
    return extent;
  }

  ipid_depth_log.FPrint("计算 min-height 和 max-height 的约束范围。");

  if (apply_automatic_min_size) {
    ipid_depth_log.FPrint(
        "由于需要应用自动最小高度，将使用 min-intrinsic 作为自动 min-height。");
  }

  MinMaxSizes min_max = ComputeMinMaxBlockSizes(
      space, node, border_padding,
      apply_automatic_min_size ? &Length::MinIntrinsic() : nullptr,
      BlockSizeFunc, override_available_size);

  ipid_depth_log.FPrint("计算得到的高度约束范围：{}",
                        ipid::GetMinMaxSizesString(min_max));

  // When fragmentation is present often want to encompass the intrinsic size.
  if (space.MinBlockSizeShouldEncompassIntrinsicSize() &&
      intrinsic_size != kIndefiniteSize) {
    ipid_depth_log.FPrint(
        "[分片场景] 需要确保最小高度包含固有高度。\n"
        "固有高度：{}\n"
        "当前最大高度：{}\n"
        "将使用两者的较小值来更新min-height。",
        intrinsic_size, min_max.max_size);
    LayoutUnit encompass_size = std::min(intrinsic_size, min_max.max_size);
    min_max.Encompass(encompass_size);
    ipid_depth_log.FPrint("更新后的高度约束范围：{}",
                          ipid::GetMinMaxSizesString(min_max));
  }

  ipid_depth_log.FPrint(
      "将主要高度 {} 限制在 min-height 和 max-height 约束范围内。", extent);

  LayoutUnit final_size = min_max.ClampSizeToMinAndMax(extent);

  ipid_depth_log.FPrint("元素 {} 的最终高度计算结果：{}",
                        ipid::GetNodeStr(node), final_size);

  return final_size;
}

}  // namespace

LayoutUnit ComputeBlockSizeForFragment(const ConstraintSpace& constraint_space,
                                       const BlockNode& node,
                                       const BoxStrut& border_padding,
                                       LayoutUnit intrinsic_size,
                                       LayoutUnit inline_size,
                                       LayoutUnit override_available_size) {
  IpidDepthLog ipid_depth_log("length_utils.cc: ComputeBlockSizeForFragment");

  ipid_depth_log.FPrint(
      "开始计算元素 {} 的片段高度（外层入口函数）。\n"
      "开辟的空间：{}\n"
      "当前元素的 border+padding：{}\n"
      "固有高度（通常为内容高度）：{}\n"
      "宽度值（用于 aspect-ratio 计算）：{}",
      ipid::GetNodeStr(node), ipid::GetConstraintSpaceString(constraint_space),
      ipid::GetBoxStrutString(border_padding), intrinsic_size, inline_size);

  if (override_available_size != kIndefiniteSize) {
    ipid_depth_log.FPrint(
        "上游传入了 override_available_size: "
        "{}，这通常用于特殊元素（如表格）。",
        override_available_size);
  }

  // The |override_available_size| should only be used for <table>s.
  DCHECK(override_available_size == kIndefiniteSize || node.IsTable());

  if (constraint_space.IsFixedBlockSize()) {
    ipid_depth_log.FPrint(
        "[快捷路径 1] 当前的 ConstraintSpace "
        "要求子元素必须使用固定高度（这种情况一般发生在 flex、grid "
        "布局中，例如设置了 align-items: stretch "
        "的情况），此时无需进行复杂的高度计算。\n");

    LayoutUnit block_size = override_available_size == kIndefiniteSize
                                ? constraint_space.AvailableSize().block_size
                                : override_available_size;

    if (override_available_size == kIndefiniteSize) {
      ipid_depth_log.FPrint("使用约束空间中的固定高度：{}", block_size);
    } else {
      ipid_depth_log.FPrint(
          "使用上游传入的 override_available_size 作为固定高度：{}",
          block_size);
    }

    if (constraint_space.MinBlockSizeShouldEncompassIntrinsicSize()) {
      ipid_depth_log.FPrint(
          "[分片场景] 需要确保高度至少包含固有高度 {}，\n"
          "最终高度为固定高度 {} 和固有高度的较大值。",
          intrinsic_size, block_size);
      LayoutUnit result = std::max(intrinsic_size, block_size);
      ipid_depth_log.FPrint("固定高度计算结果：{}", result);
      return result;
    }

    ipid_depth_log.FPrint("固定高度计算结果：{}", block_size);
    return block_size;
  }

  if (constraint_space.IsTableCell() && intrinsic_size != kIndefiniteSize) {
    ipid_depth_log.FPrint(
        "[快捷路径 2] 当前元素为表格单元格且有明确的固有高度 {}。\n"
        "根据 CSS 表格布局规范，表格单元格应直接使用其固有高度。",
        intrinsic_size);
    return intrinsic_size;
  }

  if (constraint_space.IsAnonymous()) {
    ipid_depth_log.FPrint(
        "[快捷路径 3] 当前元素为匿名盒子，直接使用固有高度 {}。\n"
        "匿名盒子是浏览器内部生成的辅助元素，通常不需要复杂的高度计算。",
        intrinsic_size);
    return intrinsic_size;
  }

  ipid_depth_log.FPrint(
      "当前 ConstraintSpace "
      "没有要求必须使用固定的高度；当前的元素也不是表格单元格，或者就算是也没有"
      "明确的固有高度；"
      "也不是匿名盒子。上述情况可以跳过计算，但若上述条件都不满足，只能调用 "
      "ComputeBlockSizeForFragmentInternal "
      "进行完整的高度解析流程。");

  LayoutUnit result = ComputeBlockSizeForFragmentInternal(
      constraint_space, node, border_padding, intrinsic_size, inline_size,
      override_available_size);

  ipid_depth_log.FPrint("元素 {} 的片段高度计算完成，最终结果：{}",
                        ipid::GetNodeStr(node), result);

  return result;
}

LayoutUnit ComputeInitialBlockSizeForFragment(
    const ConstraintSpace& space,
    const BlockNode& node,
    const BoxStrut& border_padding,
    LayoutUnit intrinsic_size,
    LayoutUnit inline_size,
    LayoutUnit override_available_size) {
  if (space.IsInitialBlockSizeIndefinite())
    return intrinsic_size;
  return ComputeBlockSizeForFragment(space, node, border_padding,
                                     intrinsic_size, inline_size,
                                     override_available_size);
}

namespace {

// Returns the default natural size.
LogicalSize ComputeDefaultNaturalSize(const BlockNode& node) {
  const auto& style = node.Style();
  PhysicalSize natural_size(LayoutUnit(300), LayoutUnit(150));
  natural_size.Scale(style.EffectiveZoom());
  return ToLogicalSize(natural_size, style.GetWritingMode());
}

// This takes the aspect-ratio, and natural-sizes and normalizes them returning
// the border-box natural-size.
//
// The following combinations are possible:
//  - an aspect-ratio with a natural-size
//  - an aspect-ratio with no natural-size
//  - no aspect-ratio with a natural-size
//
// It is not possible to have no aspect-ratio with no natural-size (as we'll
// use the default replaced size of 300x150 as a last resort).
// https://www.w3.org/TR/CSS22/visudet.html#inline-replaced-width
std::optional<LogicalSize> ComputeNormalizedNaturalSize(
    const BlockNode& node,
    const BoxStrut& border_padding,
    const EBoxSizing box_sizing,
    const LogicalSize& aspect_ratio) {
  std::optional<LayoutUnit> intrinsic_inline;
  std::optional<LayoutUnit> intrinsic_block;
  node.IntrinsicSize(&intrinsic_inline, &intrinsic_block);

  // Add the border-padding. If we *don't* have an aspect-ratio use the default
  // natural size (300x150).
  if (intrinsic_inline) {
    intrinsic_inline = *intrinsic_inline + border_padding.InlineSum();
  } else if (aspect_ratio.IsEmpty()) {
    intrinsic_inline = ComputeDefaultNaturalSize(node).inline_size +
                       border_padding.InlineSum();
  }

  if (intrinsic_block) {
    intrinsic_block = *intrinsic_block + border_padding.BlockSum();
  } else if (aspect_ratio.IsEmpty()) {
    intrinsic_block =
        ComputeDefaultNaturalSize(node).block_size + border_padding.BlockSum();
  }

  // If we have one natural size reflect via. the aspect-ratio.
  if (!intrinsic_inline && intrinsic_block) {
    DCHECK(!aspect_ratio.IsEmpty());
    intrinsic_inline = InlineSizeFromAspectRatio(border_padding, aspect_ratio,
                                                 box_sizing, *intrinsic_block);
  }
  // There are cases where the natural-size wont match the aspect-ratio. Always
  // coerce the natural block-size to respect the aspect-ratio when present.
  if (intrinsic_inline && (!intrinsic_block || !aspect_ratio.IsEmpty())) {
    DCHECK(!aspect_ratio.IsEmpty());
    intrinsic_block = BlockSizeFromAspectRatio(border_padding, aspect_ratio,
                                               box_sizing, *intrinsic_inline);
  }

  DCHECK(intrinsic_inline.has_value() == intrinsic_block.has_value());
  if (intrinsic_inline && intrinsic_block)
    return LogicalSize(*intrinsic_inline, *intrinsic_block);

  return std::nullopt;
}

// The main part of ComputeReplacedSize(). This function doesn't handle a
// case of <svg> as the documentElement.
LogicalSize ComputeReplacedSizeInternal(const BlockNode& node,
                                        const ConstraintSpace& space,
                                        const BoxStrut& border_padding,
                                        ReplacedSizeMode mode) {
  DCHECK(node.IsReplaced());

  const ComputedStyle& style = node.Style();
  const EBoxSizing box_sizing = style.BoxSizingForAspectRatio();
  const LogicalSize aspect_ratio = node.GetReplacedAspectRatio();
  const std::optional<LogicalSize> natural_size = ComputeNormalizedNaturalSize(
      node, border_padding, box_sizing, aspect_ratio);

  const Length& block_length = style.LogicalHeight();

  auto BlockSizeFunc = [&](SizeType) -> LayoutUnit {
    if (aspect_ratio.IsEmpty()) {
      DCHECK(natural_size);
      return natural_size->block_size;
    }
    if (mode == ReplacedSizeMode::kNormal) {
      return ComputeReplacedSize(node, space, border_padding,
                                 ReplacedSizeMode::kIgnoreBlockLengths)
          .block_size;
    }
    if (natural_size) {
      return natural_size->block_size;
    }
    return kIndefiniteSize;
  };

  MinMaxSizes block_min_max_sizes;
  std::optional<LayoutUnit> replaced_block;
  if (mode == ReplacedSizeMode::kIgnoreBlockLengths) {
    // Don't resolve any block lengths or constraints.
    block_min_max_sizes = {LayoutUnit(), LayoutUnit::Max()};
  } else {
    // Replaced elements in quirks-mode resolve their min/max block-sizes
    // against a different size than the main size. See:
    //  - https://www.w3.org/TR/CSS21/visudet.html#min-max-heights
    //  - https://bugs.chromium.org/p/chromium/issues/detail?id=385877
    // For the history on this behavior. Fortunately if this is the case we can
    // just use the given available size to resolve these sizes against.
    const LayoutUnit min_max_percentage_resolution_size =
        node.GetDocument().InQuirksMode() && !node.IsOutOfFlowPositioned()
            ? space.AvailableSize().block_size
            : space.PercentageResolutionBlockSize();

    block_min_max_sizes = {
        ResolveMinBlockLength(space, style, border_padding, BlockSizeFunc,
                              style.LogicalMinHeight(),
                              /* auto_length */ nullptr,
                              /* override_available_size */ kIndefiniteSize,
                              &min_max_percentage_resolution_size),
        ResolveMaxBlockLength(space, style, border_padding,
                              style.LogicalMaxHeight(), BlockSizeFunc,
                              /* override_available_size */ kIndefiniteSize,
                              &min_max_percentage_resolution_size)};
    block_min_max_sizes.max_size =
        std::max(block_min_max_sizes.min_size, block_min_max_sizes.max_size);

    if (space.IsFixedBlockSize()) {
      replaced_block = space.AvailableSize().block_size;
      DCHECK_GE(*replaced_block, 0);
    } else {
      const Length& auto_block_length = space.IsBlockAutoBehaviorStretch()
                                            ? Length::Stretch()
                                            : Length::FitContent();
      const LayoutUnit block_size =
          ResolveMainBlockLength(space, style, border_padding, block_length,
                                 &auto_block_length, BlockSizeFunc);
      if (block_size != kIndefiniteSize) {
        DCHECK_GE(block_size, LayoutUnit());
        replaced_block = block_min_max_sizes.ClampSizeToMinAndMax(block_size);
      }
    }
  }

  // We can only compute the transferred min/max sizes if we have an
  // aspect-ratio.
  const MinMaxSizes transferred_min_max_sizes =
      aspect_ratio.IsEmpty()
          ? MinMaxSizes{LayoutUnit(), LayoutUnit::Max()}
          : ComputeTransferredMinMaxInlineSizes(
                aspect_ratio, block_min_max_sizes, border_padding, box_sizing);

  const Length& inline_length = style.LogicalWidth();

  auto MinMaxSizesFunc = [&](SizeType) -> MinMaxSizesResult {
    LayoutUnit size;
    if (aspect_ratio.IsEmpty()) {
      DCHECK(natural_size);
      size = natural_size->inline_size;
    } else if (replaced_block) {
      size = InlineSizeFromAspectRatio(border_padding, aspect_ratio, box_sizing,
                                       *replaced_block);
    } else if (natural_size) {
      DCHECK_NE(mode, ReplacedSizeMode::kIgnoreInlineLengths);
      size = mode == ReplacedSizeMode::kNormal
                 ? ComputeReplacedSize(node, space, border_padding,
                                       ReplacedSizeMode::kIgnoreInlineLengths)
                       .inline_size
                 : natural_size->inline_size;
    } else {
      // We don't have a natural size.
      size = kIndefiniteSize;
    }

    // |depends_on_block_constraints| doesn't matter in this context.
    MinMaxSizes sizes;
    sizes.min_size = sizes.max_size = size;
    return {sizes, /* depends_on_block_constraints */ false};
  };

  MinMaxSizes inline_min_max_sizes;
  std::optional<LayoutUnit> replaced_inline;
  if (mode == ReplacedSizeMode::kIgnoreInlineLengths) {
    // Just use the transferred sizes.
    inline_min_max_sizes = transferred_min_max_sizes;
  } else {
    inline_min_max_sizes = {
        ResolveMinInlineLength(space, style, border_padding, MinMaxSizesFunc,
                               style.LogicalMinWidth()),
        ResolveMaxInlineLength(space, style, border_padding, MinMaxSizesFunc,
                               style.LogicalMaxWidth())};

    // Transfer the block min/max sizes if applicable.
    if (style.LogicalWidth().HasAuto() &&
        space.InlineAutoBehavior() != AutoSizeBehavior::kStretchExplicit) {
      // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-size-transfers
      inline_min_max_sizes.min_size =
          std::max(inline_min_max_sizes.min_size,
                   std::min(transferred_min_max_sizes.min_size,
                            inline_min_max_sizes.max_size));
      inline_min_max_sizes.max_size = std::min(
          inline_min_max_sizes.max_size, transferred_min_max_sizes.max_size);
    }

    // Ensure the max-size encompasses the min-size.
    inline_min_max_sizes.max_size =
        std::max(inline_min_max_sizes.min_size, inline_min_max_sizes.max_size);

    if (space.IsFixedInlineSize()) {
      replaced_inline = space.AvailableSize().inline_size;
      DCHECK_GE(*replaced_inline, 0);
    } else {
      const Length& auto_length = space.IsInlineAutoBehaviorStretch()
                                      ? Length::Stretch()
                                      : Length::FitContent();
      const LayoutUnit inline_size =
          ResolveMainInlineLength(space, style, border_padding, MinMaxSizesFunc,
                                  inline_length, &auto_length);
      if (inline_size != kIndefiniteSize) {
        DCHECK_GE(inline_size, LayoutUnit());
        replaced_inline =
            inline_min_max_sizes.ClampSizeToMinAndMax(inline_size);
      }
    }
  }

  if (replaced_inline && replaced_block)
    return LogicalSize(*replaced_inline, *replaced_block);

  auto StretchFit = [&]() -> LayoutUnit {
    // Stretch to the available-size if it is definite.
    if (space.AvailableSize().inline_size != kIndefiniteSize) {
      return ResolveMainInlineLength(
          space, style, border_padding,
          [](SizeType) -> MinMaxSizesResult { NOTREACHED(); },
          Length::Stretch(), /* auto_length */ nullptr,
          /* override_available_size */ kIndefiniteSize);
    }

    // All browsers now use the default natural-size for a percentage.
    if (inline_length.HasPercent()) {
      return ComputeDefaultNaturalSize(node).inline_size +
             border_padding.InlineSum();
    }

    return border_padding.InlineSum();
  };

  // We have *only* an aspect-ratio with no sizes (natural or otherwise), we
  // default to stretching.
  if (!natural_size && !replaced_inline && !replaced_block) {
    replaced_inline = inline_min_max_sizes.ClampSizeToMinAndMax(StretchFit());
  }

  // We only know one size, the other gets computed via the aspect-ratio (if
  // present), or defaults to the natural-size.
  if (replaced_inline) {
    DCHECK(!replaced_block);
    DCHECK(natural_size || !aspect_ratio.IsEmpty());
    replaced_block = aspect_ratio.IsEmpty() ? natural_size->block_size
                                            : BlockSizeFromAspectRatio(
                                                  border_padding, aspect_ratio,
                                                  box_sizing, *replaced_inline);
    replaced_block = block_min_max_sizes.ClampSizeToMinAndMax(*replaced_block);
    return LogicalSize(*replaced_inline, *replaced_block);
  }

  if (replaced_block) {
    DCHECK(!replaced_inline);
    DCHECK(natural_size || !aspect_ratio.IsEmpty());
    replaced_inline = aspect_ratio.IsEmpty() ? natural_size->inline_size
                                             : InlineSizeFromAspectRatio(
                                                   border_padding, aspect_ratio,
                                                   box_sizing, *replaced_block);
    replaced_inline =
        inline_min_max_sizes.ClampSizeToMinAndMax(*replaced_inline);
    return LogicalSize(*replaced_inline, *replaced_block);
  }

  // Both sizes are unknown - use the natural-size.
  return {inline_min_max_sizes.ClampSizeToMinAndMax(natural_size->inline_size),
          block_min_max_sizes.ClampSizeToMinAndMax(natural_size->block_size)};
}

}  // namespace

// Computes size for a replaced element.
LogicalSize ComputeReplacedSize(const BlockNode& node,
                                const ConstraintSpace& space,
                                const BoxStrut& border_padding,
                                ReplacedSizeMode mode) {
  DCHECK(node.IsReplaced());

  const auto* svg_root = DynamicTo<LayoutSVGRoot>(node.GetLayoutBox());
  if (!svg_root || !svg_root->IsDocumentElement()) {
    return ComputeReplacedSizeInternal(node, space, border_padding, mode);
  }

  PhysicalSize container_size(svg_root->GetContainerSize());
  if (!container_size.IsEmpty()) {
    LogicalSize size =
        ToLogicalSize(container_size, node.Style().GetWritingMode());
    size.inline_size += border_padding.InlineSum();
    size.block_size += border_padding.BlockSum();
    return size;
  }

  if (svg_root->IsEmbeddedThroughFrameContainingSVGDocument()) {
    LogicalSize size = space.AvailableSize();
    size.block_size = node.Style().IsHorizontalWritingMode()
                          ? node.InitialContainingBlockSize().height
                          : node.InitialContainingBlockSize().width;
    return size;
  }

  LogicalSize size =
      ComputeReplacedSizeInternal(node, space, border_padding, mode);

  if (node.Style().LogicalWidth().HasPercent()) {
    double factor = svg_root->LogicalSizeScaleFactorForPercentageLengths();
    if (factor != 1.0) {
      // TODO(https://crbug.com/313072): Just because a calc *has* percentages
      // doesn't mean *all* the lengths are percentages.
      size.inline_size *= factor;
    }
  }

  const Length& logical_height = node.Style().LogicalHeight();
  if (logical_height.HasPercent()) {
    // TODO(https://crbug.com/313072): Might this also be needed for intrinsic
    // sizing keywords?
    LayoutUnit height = ValueForLength(
        logical_height,
        node.GetDocument().GetLayoutView()->ViewLogicalHeightForPercentages());
    double factor = svg_root->LogicalSizeScaleFactorForPercentageLengths();
    if (factor != 1.0) {
      // TODO(https://crbug.com/313072): Just because a calc *has* percentages
      // doesn't mean *all* the lengths are percentages.
      height *= factor;
    }
    size.block_size = height;
  }
  return size;
}

int ResolveUsedColumnCount(int computed_count,
                           LayoutUnit computed_size,
                           LayoutUnit used_gap,
                           LayoutUnit available_size) {
  if (computed_size == kIndefiniteSize) {
    if (!computed_count) {
      // Both `column-width` and `column-count` are auto. We're here because
      // `column-height` is non-auto. Set column count to 1.
      return 1;
    }
    return computed_count;
  }
  DCHECK_GT(computed_size, LayoutUnit());
  int count_from_width =
      ((available_size + used_gap) / (computed_size + used_gap)).ToInt();
  count_from_width = std::max(1, count_from_width);
  if (!computed_count)
    return count_from_width;
  return std::max(1, std::min(computed_count, count_from_width));
}

int ResolveUsedColumnCount(const ComputedStyle& style,
                           LayoutUnit available_size) {
  LayoutUnit computed_column_inline_size =
      style.HasAutoColumnWidth()
          ? kIndefiniteSize
          : std::max(LayoutUnit(1), LayoutUnit(style.ColumnWidth()));
  LayoutUnit gap = ResolveColumnGapForMulticol(style, available_size);
  int computed_count = style.HasAutoColumnCount() ? 0 : style.ColumnCount();
  return ResolveUsedColumnCount(computed_count, computed_column_inline_size,
                                gap, available_size);
}

LayoutUnit ResolveUsedColumnInlineSize(int computed_count,
                                       LayoutUnit computed_size,
                                       LayoutUnit used_gap,
                                       LayoutUnit available_size) {
  int used_count = ResolveUsedColumnCount(computed_count, computed_size,
                                          used_gap, available_size);
  return std::max(((available_size + used_gap) / used_count) - used_gap,
                  LayoutUnit());
}

LayoutUnit ResolveUsedColumnInlineSize(const ComputedStyle& style,
                                       LayoutUnit available_size) {
  // Should only attempt to resolve this if columns != auto.
  DCHECK(style.SpecifiesColumns());

  LayoutUnit computed_size =
      style.HasAutoColumnWidth()
          ? kIndefiniteSize
          : std::max(LayoutUnit(1), LayoutUnit(style.ColumnWidth()));
  int computed_count = style.HasAutoColumnCount() ? 0 : style.ColumnCount();
  LayoutUnit used_gap = ResolveColumnGapForMulticol(style, available_size);
  return ResolveUsedColumnInlineSize(computed_count, computed_size, used_gap,
                                     available_size);
}

std::optional<LayoutUnit> ResolveColumnGapLength(const ComputedStyle& style,
                                                 LayoutUnit available_size) {
  if (const std::optional<Length>& gap = style.ColumnGap()) {
    return MinimumValueForLength(*gap, available_size.ClampIndefiniteToZero());
  }
  return std::nullopt;
}

LayoutUnit ResolveColumnGapForMulticol(const ComputedStyle& style,
                                       LayoutUnit available_size) {
  return ResolveColumnGapLength(style, available_size)
      .value_or(LayoutUnit(style.GetFontDescription().ComputedPixelSize()));
}

std::optional<LayoutUnit> ResolveRowGapLength(const ComputedStyle& style,
                                              LayoutUnit available_size) {
  if (const std::optional<Length>& gap = style.RowGap()) {
    return MinimumValueForLength(*gap, available_size.ClampIndefiniteToZero());
  }
  return std::nullopt;
}

LayoutUnit ResolveRowGapForMulticol(const ComputedStyle& style,
                                    LayoutUnit available_size) {
  return ResolveRowGapLength(style, available_size)
      .value_or(LayoutUnit(style.GetFontDescription().ComputedPixelSize()));
}

std::optional<LayoutUnit> ResolveItemToleranceLength(
    const ComputedStyle& style,
    LayoutUnit available_size) {
  // TODO (celestepan): Account for when item-tolerance is set to infinite.
  const ItemTolerance& item_tolerance = style.GetItemTolerance();
  if (item_tolerance.IsNormal()) {
    return std::nullopt;
  }
  if (item_tolerance.IsInfinite()) {
    return LayoutUnit::Max();
  }
  return MinimumValueForLength(item_tolerance.GetLength(),
                               available_size.ClampIndefiniteToZero());
}

LayoutUnit ResolveItemToleranceForMasonry(const ComputedStyle& style,
                                          const LogicalSize& available_size) {
  return ResolveItemToleranceLength(
             style, (style.MasonryTrackSizingDirection() == kForColumns)
                        ? available_size.block_size
                        : available_size.inline_size)
      .value_or(LayoutUnit(style.GetFontDescription().ComputedPixelSize()));
}

LayoutUnit ColumnInlineProgression(const ComputedStyle& style,
                                   LayoutUnit available_size) {
  return ResolveUsedColumnInlineSize(style, available_size) +
         ResolveColumnGapForMulticol(style, available_size);
}

PhysicalBoxStrut ComputePhysicalMargins(
    const ComputedStyle& style,
    PhysicalSize percentage_resolution_size) {
  if (!style.MayHaveMargin())
    return PhysicalBoxStrut();

  return PhysicalBoxStrut(
      MinimumValueForLength(style.MarginTop(),
                            percentage_resolution_size.height),
      MinimumValueForLength(style.MarginRight(),
                            percentage_resolution_size.width),
      MinimumValueForLength(style.MarginBottom(),
                            percentage_resolution_size.height),
      MinimumValueForLength(style.MarginLeft(),
                            percentage_resolution_size.width));
}

BoxStrut ComputeMarginsFor(const ConstraintSpace& constraint_space,
                           const ComputedStyle& style,
                           const ConstraintSpace& compute_for) {
  if (!style.MayHaveMargin() || constraint_space.IsAnonymous())
    return BoxStrut();
  LogicalSize percentage_resolution_size =
      constraint_space.MarginPaddingPercentageResolutionSize();
  return ComputePhysicalMargins(style, percentage_resolution_size)
      .ConvertToLogical(compute_for.GetWritingDirection());
}

namespace {

BoxStrut ComputeBordersInternal(const ComputedStyle& style) {
  return PhysicalBoxStrut::FromInts(
             style.BorderTopWidth(), style.BorderRightWidth(),
             style.BorderBottomWidth(), style.BorderLeftWidth())
      .ConvertToLogical(style.GetWritingDirection());
}

}  // namespace

BoxStrut ComputeBorders(const ConstraintSpace& constraint_space,
                        const BlockNode& node) {
  // If we are producing an anonymous fragment (e.g. a column), it has no
  // borders, padding or scrollbars. Using the ones from the container can only
  // cause trouble.
  if (constraint_space.IsAnonymous())
    return BoxStrut();

  // If we are a table cell we just access the values set by the parent table
  // layout as border may be collapsed etc.
  if (constraint_space.IsTableCell())
    return constraint_space.TableCellBorders();

  if (node.IsTable()) {
    return To<TableNode>(node).GetTableBorders()->TableBorder();
  }

  return ComputeBordersInternal(node.Style());
}

BoxStrut ComputeBordersForInline(const ComputedStyle& style) {
  return ComputeBordersInternal(style);
}

BoxStrut ComputeNonCollapsedTableBorders(const ComputedStyle& style) {
  return ComputeBordersInternal(style);
}

BoxStrut ComputeBordersForTest(const ComputedStyle& style) {
  return ComputeBordersInternal(style);
}

BoxStrut ComputePadding(const ConstraintSpace& constraint_space,
                        const ComputedStyle& style) {
  // If we are producing an anonymous fragment (e.g. a column) we shouldn't
  // have any padding.
  if (!style.MayHavePadding() || constraint_space.IsAnonymous())
    return BoxStrut();

  // Tables with collapsed borders don't have any padding.
  if (style.IsDisplayTableBox() &&
      style.BorderCollapse() == EBorderCollapse::kCollapse) {
    return BoxStrut();
  }

  // This function may be called for determining intrinsic padding, clamp
  // indefinite %-sizes to zero. See:
  // https://drafts.csswg.org/css-sizing-3/#min-percentage-contribution
  LogicalSize percentage_resolution_size =
      constraint_space.MarginPaddingPercentageResolutionSize()
          .ClampIndefiniteToZero();
  return {MinimumValueForLength(style.PaddingInlineStart(),
                                percentage_resolution_size.inline_size),
          MinimumValueForLength(style.PaddingInlineEnd(),
                                percentage_resolution_size.inline_size),
          MinimumValueForLength(style.PaddingBlockStart(),
                                percentage_resolution_size.block_size),
          MinimumValueForLength(style.PaddingBlockEnd(),
                                percentage_resolution_size.block_size)};
}

BoxStrut ComputeScrollbarsForNonAnonymous(const BlockNode& node) {
  const ComputedStyle& style = node.Style();
  if (!style.IsScrollContainer() && style.IsScrollbarGutterAuto())
    return BoxStrut();
  const LayoutBox* layout_box = node.GetLayoutBox();
  return layout_box->ComputeLogicalScrollbars();
}

void ResolveInlineAutoMargins(const ComputedStyle& style,
                              const ComputedStyle& container_style,
                              LayoutUnit available_inline_size,
                              LayoutUnit inline_size,
                              BoxStrut* margins) {
  const LayoutUnit used_space = inline_size + margins->InlineSum();
  const LayoutUnit available_space = available_inline_size - used_space;
  bool is_start_auto = style.MarginInlineStartUsing(container_style).IsAuto();
  bool is_end_auto = style.MarginInlineEndUsing(container_style).IsAuto();
  if (is_start_auto && is_end_auto) {
    margins->inline_start = (available_space / 2).ClampNegativeToZero();
    margins->inline_end =
        available_inline_size - inline_size - margins->inline_start;
  } else if (is_start_auto) {
    margins->inline_start = available_space.ClampNegativeToZero();
  } else if (is_end_auto) {
    margins->inline_end =
        available_inline_size - inline_size - margins->inline_start;
  }
}

void ResolveAutoMargins(Length start_length,
                        Length end_length,
                        LayoutUnit additional_space,
                        LayoutUnit* start_result,
                        LayoutUnit* end_result) {
  bool start_is_auto = start_length.IsAuto();
  bool end_is_auto = end_length.IsAuto();
  if (start_is_auto) {
    if (end_is_auto) {
      *start_result = additional_space / 2;
      additional_space -= *start_result;
    } else {
      *start_result = additional_space;
    }
  }
  if (end_is_auto) {
    *end_result = additional_space;
  }
}

void ResolveAutoMargins(Length inline_start_length,
                        Length inline_end_length,
                        Length block_start_length,
                        Length block_end_length,
                        LayoutUnit additional_inline_space,
                        LayoutUnit additional_block_space,
                        BoxStrut* margins) {
  ResolveAutoMargins(inline_start_length, inline_end_length,
                     additional_inline_space, &margins->inline_start,
                     &margins->inline_end);
  ResolveAutoMargins(block_start_length, block_end_length,
                     additional_block_space, &margins->block_start,
                     &margins->block_end);
}

LayoutUnit LineOffsetForTextAlign(ETextAlign text_align,
                                  TextDirection direction,
                                  LayoutUnit space_left) {
  bool is_ltr = IsLtr(direction);
  if (text_align == ETextAlign::kStart || text_align == ETextAlign::kJustify ||
      text_align == ETextAlign::kMatchParent) {
    text_align = is_ltr ? ETextAlign::kLeft : ETextAlign::kRight;
  } else if (text_align == ETextAlign::kEnd) {
    text_align = is_ltr ? ETextAlign::kRight : ETextAlign::kLeft;
  }

  switch (text_align) {
    case ETextAlign::kLeft:
    case ETextAlign::kWebkitLeft: {
      // The direction of the block should determine what happens with wide
      // lines. In particular with RTL blocks, wide lines should still spill
      // out to the left.
      if (is_ltr)
        return LayoutUnit();
      return space_left.ClampPositiveToZero();
    }
    case ETextAlign::kRight:
    case ETextAlign::kWebkitRight: {
      // In RTL, trailing spaces appear on the left of the line.
      if (!is_ltr) [[unlikely]] {
        return space_left;
      }
      // Wide lines spill out of the block based off direction.
      // So even if text-align is right, if direction is LTR, wide lines
      // should overflow out of the right side of the block.
      if (space_left > LayoutUnit())
        return space_left;
      return LayoutUnit();
    }
    case ETextAlign::kCenter:
    case ETextAlign::kWebkitCenter: {
      if (is_ltr)
        return (space_left / 2).ClampNegativeToZero();
      // In RTL, trailing spaces appear on the left of the line.
      if (space_left > LayoutUnit())
        return (space_left / 2).ClampNegativeToZero();
      // In RTL, wide lines should spill out to the left, same as kRight.
      return space_left;
    }
    default:
      NOTREACHED();
  }
}

// Calculates default content size for html and body elements in quirks mode.
// Returns |kIndefiniteSize| in all other cases.
LayoutUnit CalculateDefaultBlockSize(const ConstraintSpace& space,
                                     const BlockNode& node,
                                     const BlockBreakToken* break_token,
                                     const BoxStrut& border_scrollbar_padding) {
  // In quirks mode, html and body elements will completely fill the ICB, block
  // percentages should resolve against this size.
  if (node.IsQuirkyAndFillsViewport() && !IsBreakInside(break_token)) {
    LayoutUnit block_size = space.AvailableSize().block_size;
    block_size -= ComputeMarginsForSelf(space, node.Style()).BlockSum();
    return std::max(block_size.ClampNegativeToZero(),
                    border_scrollbar_padding.BlockSum());
  }
  return kIndefiniteSize;
}

FragmentGeometry CalculateInitialFragmentGeometry(
    const ConstraintSpace& space,
    const BlockNode& node,
    const BlockBreakToken* break_token,
    MinMaxSizesFunctionRef min_max_sizes_func,
    bool is_intrinsic) {
  IpidDepthLog ipid_depth_log(
      "length_utils.cc: CalculateInitialFragmentGeometry");
  std::string node_str = ipid::GetNodeStr(node);
  const auto& style = node.Style();

  if (node.IsFrameSet()) {
    if (node.IsParentNGFrameSet()) {
      const auto size = space.AvailableSize();
      DCHECK_NE(size.inline_size, kIndefiniteSize);
      DCHECK_NE(size.block_size, kIndefiniteSize);
      DCHECK(space.IsFixedInlineSize());
      DCHECK(space.IsFixedBlockSize());
      return {size, {}, {}, {}};
    }

    const auto size = node.InitialContainingBlockSize();
    return {ToLogicalSize(size, style.GetWritingMode()), {}, {}, {}};
  }

  ipid_depth_log.FPrint("正在计算元素 {} 的 border 尺寸。", node_str);
  const auto border = ComputeBorders(space, node);
  ipid_depth_log.FPrint("border 尺寸为 {}\n\n正在计算元素 {} 的 padding 尺寸。",
                        ipid::GetBoxStrutString(border), node_str);
  const auto padding = ComputePadding(space, style);
  ipid_depth_log.FPrint(
      "padding 尺寸为 {}\n\n正在计算元素 {} 的滚动条所占空间。",
      ipid::GetBoxStrutString(padding), node_str);
  auto scrollbar = ComputeScrollbars(space, node);
  ipid_depth_log.FPrint("滚动条所占空间为 {}",
                        ipid::GetBoxStrutString(scrollbar));

  const auto border_padding = border + padding;
  const auto border_scrollbar_padding = border_padding + scrollbar;

  if (node.IsReplaced()) {
    ipid_depth_log.FPrint(
        "元素 {} 是替换元素，正在调用 ComputeReplacedSize 计算其 border-box "
        "的尺寸。",
        node_str);
    const auto border_box_size = ComputeReplacedSize(
        node, space, border_padding,
        is_intrinsic ? ReplacedSizeMode::kIgnoreInlineLengths
                     : ReplacedSizeMode::kNormal);
    ipid_depth_log.FPrint("替换元素 {} 的 border-box 的尺寸为 {}", node_str,
                          border_box_size);
    return {border_box_size, border, scrollbar, padding};
  }

  ipid_depth_log.FPrint(
      "正在调用 ComputeInlineSizeForFragment 计算元素 {} 的宽度。", node_str);
  const LayoutUnit inline_size =
      is_intrinsic ? kIndefiniteSize
                   : ComputeInlineSizeForFragment(space, node, border_padding,
                                                  min_max_sizes_func);

  if (is_intrinsic) {
    ipid_depth_log.FPrint("当前为固有尺寸计算模式，宽度设为不确定值 (-1)。");
  } else {
    ipid_depth_log.FPrint("ComputeInlineSizeForFragment 返回的宽度为：{}px",
                          inline_size);
  }

  if (inline_size != kIndefiniteSize &&
      inline_size < border_scrollbar_padding.InlineSum() &&
      scrollbar.InlineSum() && !space.IsAnonymous()) [[unlikely]] {
    ipid_depth_log.FPrint(
        "[特殊情况] 检测到宽度 ({}px) 小于 border+scrollbar+padding "
        "({}px)，且有滚动条且非匿名空间，需要调整滚动条尺寸以防止布局问题。",
        inline_size, border_scrollbar_padding.InlineSum());

    // Clamp the inline size of the scrollbar, unless it's larger than the
    // inline size of the content box, in which case we'll return that instead.
    // Scrollbar handling is quite bad in such situations, and this method here
    // is just to make sure that left-hand scrollbars don't mess up scrollWidth.
    // For the full story, visit http://crbug.com/724255.
    const auto content_box_inline_size =
        inline_size - border_padding.InlineSum();

    ipid_depth_log.FPrint("内容盒子的宽度为：{}px", content_box_inline_size);

    if (scrollbar.InlineSum() > content_box_inline_size) {
      ipid_depth_log.FPrint(
          "滚动条尺寸 ({}px) 大于内容盒子宽度 ({}px)，需要缩小滚动条。",
          scrollbar.InlineSum(), content_box_inline_size);

      if (scrollbar.inline_end) {
        DCHECK(!scrollbar.inline_start);
        ipid_depth_log.FPrint("调整右侧滚动条从 {}px 到 {}px",
                              scrollbar.inline_end, content_box_inline_size);
        scrollbar.inline_end = content_box_inline_size;
      } else {
        DCHECK(scrollbar.inline_start);
        ipid_depth_log.FPrint("调整左侧滚动条从 {}px 到 {}px",
                              scrollbar.inline_start, content_box_inline_size);
        scrollbar.inline_start = content_box_inline_size;
      }
    } else {
      ipid_depth_log.FPrint("滚动条尺寸在合理范围内，无需调整。");
    }
  }

  ipid_depth_log.FPrint("正在调用 CalculateDefaultBlockSize 计算默认高度。");
  const auto default_block_size = CalculateDefaultBlockSize(
      space, node, break_token, border_scrollbar_padding);
  ipid_depth_log.FPrint("默认高度为：{}", default_block_size);

  ipid_depth_log.FPrint(
      "正在调用 ComputeInitialBlockSizeForFragment 计算最终高度。\n"
      "传入的宽度为：{}px\n"
      "传入的默认高度为：{}px",
      inline_size, default_block_size);
  const auto block_size = ComputeInitialBlockSizeForFragment(
      space, node, border_padding, default_block_size, inline_size);
  ipid_depth_log.FPrint("计算得到的最终高度为：{}px", block_size);

  ipid_depth_log.FPrint(
      "元素 {} 的初始尺寸（FragmentGeometry）计算完成：\n"
      "- 尺寸：宽度 {}px，高度 {}px\n"
      "- border：{}\n"
      "- scrollbar：{}\n"
      "- padding：{}",
      node_str, inline_size, block_size, ipid::GetBoxStrutString(border),
      ipid::GetBoxStrutString(scrollbar), ipid::GetBoxStrutString(padding));

  return {LogicalSize(inline_size, block_size), border, scrollbar, padding};
}

FragmentGeometry CalculateInitialFragmentGeometry(
    const ConstraintSpace& space,
    const BlockNode& node,
    const BlockBreakToken* break_token,
    bool is_intrinsic) {
  auto MinMaxSizesFunc = [&](SizeType type) -> MinMaxSizesResult {
    return node.ComputeMinMaxSizes(space.GetWritingMode(), type, space);
  };

  return CalculateInitialFragmentGeometry(space, node, break_token,
                                          MinMaxSizesFunc, is_intrinsic);
}

LogicalSize ShrinkLogicalSize(LogicalSize size, const BoxStrut& insets) {
  if (size.inline_size != kIndefiniteSize) {
    size.inline_size =
        (size.inline_size - insets.InlineSum()).ClampNegativeToZero();
  }
  if (size.block_size != kIndefiniteSize) {
    size.block_size =
        (size.block_size - insets.BlockSum()).ClampNegativeToZero();
  }

  return size;
}

LogicalSize CalculateChildAvailableSize(
    const ConstraintSpace& space,
    const BlockNode& node,
    const LogicalSize border_box_size,
    const BoxStrut& border_scrollbar_padding) {
  IpidDepthLog ipid_depth_log("length_utils.cc: CalculateChildAvailableSize");
  std::string ipid_node_str = ipid::GetNodeStr(node);
  ipid_depth_log.FPrint(
      "正在计算子元素的可用尺寸。\n"
      "父容器约束空间：{}\n"
      "子元素：{}\n"
      "子元素的 border-box 尺寸：{}\n"
      "子元素的 border+scrollbar+padding 尺寸：{}",
      ipid::GetConstraintSpaceString(space), ipid_node_str,
      ipid::GetLogicalSizeString(border_box_size),
      ipid::GetBoxStrutString(border_scrollbar_padding));

  LogicalSize child_available_size =
      ShrinkLogicalSize(border_box_size, border_scrollbar_padding);

  ipid_depth_log.FPrint(
      "我们需要将子元素 {} 的 border-box 尺寸减去其 border、滚动条和 padding "
      "尺寸。减去后，得到的子元素的可用尺寸为：{}",
      ipid_node_str, ipid::GetLogicalSizeString(child_available_size));

  if (space.IsAnonymous() ||
      (node.IsAnonymousBlockFlow() &&
       child_available_size.block_size == kIndefiniteSize)) {
    ipid_depth_log.FPrint(
        "由于节点 {} "
        "为匿名节点，且当前计算的可用尺寸中的高度为不确定值（-"
        "1），此时我们将可用尺寸的高度值直接设为当前 ConstraintSpace "
        "的可用高度 {}px。",
        ipid_node_str, space.AvailableSize().block_size);
    child_available_size.block_size = space.AvailableSize().block_size;
  }

  ipid_depth_log.FPrint("最终计算得到的子元素的可用尺寸为：{}",
                        ipid::GetLogicalSizeString(child_available_size));

  return child_available_size;
}

namespace {

// Implements the common part of the child percentage size calculation. Deals
// with how percentages are propagated from parent to child in quirks mode.
LogicalSize AdjustChildPercentageSize(const ConstraintSpace& space,
                                      const BlockNode node,
                                      LogicalSize child_percentage_size,
                                      LayoutUnit parent_percentage_block_size) {
  // In quirks mode the percentage resolution height is passed from parent to
  // child.
  // https://quirks.spec.whatwg.org/#the-percentage-height-calculation-quirk
  if (child_percentage_size.block_size == kIndefiniteSize &&
      node.UseParentPercentageResolutionBlockSizeForChildren())
    child_percentage_size.block_size = parent_percentage_block_size;

  return child_percentage_size;
}

}  // namespace

LogicalSize CalculateChildPercentageSize(
    const ConstraintSpace& space,
    const BlockNode node,
    const LogicalSize child_available_size) {
  // Anonymous block or spaces should use the parent percent block-size.
  if (space.IsAnonymous() || node.IsAnonymousBlockFlow()) {
    return {child_available_size.inline_size,
            space.PercentageResolutionBlockSize()};
  }

  // Table cell children don't apply the "percentage-quirk". I.e. if their
  // percentage resolution block-size is indefinite, they don't pass through
  // their parent's percentage resolution block-size.
  if (space.IsTableCellChild())
    return child_available_size;

  return AdjustChildPercentageSize(space, node, child_available_size,
                                   space.PercentageResolutionBlockSize());
}

LogicalSize CalculateReplacedChildPercentageSize(
    const ConstraintSpace& space,
    const BlockNode node,
    const LogicalSize child_available_size,
    const BoxStrut& border_scrollbar_padding,
    const BoxStrut& border_padding) {
  // Anonymous block or spaces should use the parent percent block-size.
  if (space.IsAnonymous() || node.IsAnonymousBlockFlow()) {
    return {child_available_size.inline_size,
            space.ReplacedChildPercentageResolutionBlockSize()};
  }

  // Table cell children don't apply the "percentage-quirk". I.e. if their
  // percentage resolution block-size is indefinite, they don't pass through
  // their parent's percentage resolution block-size.
  if (space.IsTableCellChild())
    return child_available_size;

  // Replaced descendants of a table-cell which has a definite block-size,
  // always resolve their percentages against this size (even during the
  // "layout" pass where the fixed block-size may be different).
  //
  // This ensures that between the table-cell "measure" and "layout" passes
  // the replaced descendants remain the same size.
  if (space.IsTableCell() && node.Style().LogicalHeight().IsFixed()) {
    LayoutUnit block_size = ComputeBlockSizeForFragmentInternal(
        space, node, border_padding, kIndefiniteSize /* intrinsic_size */,
        kIndefiniteSize /* inline_size */);
    DCHECK_NE(block_size, kIndefiniteSize);
    return {child_available_size.inline_size,
            (block_size - border_scrollbar_padding.BlockSum())
                .ClampNegativeToZero()};
  }

  return AdjustChildPercentageSize(
      space, node, child_available_size,
      space.ReplacedChildPercentageResolutionBlockSize());
}

LayoutUnit ClampIntrinsicBlockSize(
    const ConstraintSpace& space,
    const BlockNode& node,
    const BlockBreakToken* break_token,
    const BoxStrut& border_scrollbar_padding,
    LayoutUnit current_intrinsic_block_size,
    std::optional<LayoutUnit> body_margin_block_sum) {
  // Tables don't respect size containment, or apply the "fill viewport" quirk.
  DCHECK(!node.IsTable());

  const LayoutUnit intrinsic_block_size =
      CalculateIntrinsicBlockSizeIgnoringChildren(
          node, border_scrollbar_padding,
          /*children_have_geometry=*/true);
  if (intrinsic_block_size != kIndefiniteSize) {
    return intrinsic_block_size;
  }

  // Apply the "fills viewport" quirk if needed.
  const ComputedStyle& style = node.Style();
  if (!IsBreakInside(break_token) && node.IsQuirkyAndFillsViewport() &&
      style.LogicalHeight().IsAuto() &&
      space.AvailableSize().block_size != kIndefiniteSize) {
    DCHECK_EQ(node.IsBody() && !node.CreatesNewFormattingContext(),
              body_margin_block_sum.has_value());
    LayoutUnit margin_sum = body_margin_block_sum.value_or(
        ComputeMarginsForSelf(space, style).BlockSum());
    current_intrinsic_block_size = std::max(
        current_intrinsic_block_size,
        (space.AvailableSize().block_size - margin_sum).ClampNegativeToZero());
  }

  return current_intrinsic_block_size;
}

std::optional<MinMaxSizesResult> CalculateMinMaxSizesIgnoringChildren(
    const BlockNode& node,
    const BoxStrut& border_scrollbar_padding) {
  std::string ipid_node_str = ipid::GetNodeStr(node);
  IpidDepthLog ipid_depth_log(
      "length_utils.cc: CalculateMinMaxSizesIgnoringChildren");
  ipid_depth_log.FPrint(
      "正在判断元素 {} 是否必须在不考虑子元素的情况下计算固有尺寸。\n元素的 "
      "border+scrollbar+padding 尺寸为：{}",
      ipid_node_str, ipid::GetBoxStrutString(border_scrollbar_padding));

  MinMaxSizes sizes;
  sizes += border_scrollbar_padding.InlineSum();

  // Check if the intrinsic size was overridden.
  const LayoutUnit override_size = node.OverrideIntrinsicContentInlineSize();
  if (override_size != kIndefiniteSize) {
    ipid_depth_log.FPrint(
        "元素 {} 设置了 OverrideIntrinsicContentInlineSize = "
        "{}，导致其固有宽度 = (border + 滚动条 + padding){} + "
        "OverrideIntrinsicContentInlineSize，不依赖子元素。",
        ipid_node_str, override_size, border_scrollbar_padding.InlineSum());
    sizes += override_size;
    return MinMaxSizesResult{sizes, /* depends_on_block_constraints */ false};
  }

  // Check if we have a "default" size (a <textarea>).
  const LayoutUnit default_size = node.DefaultIntrinsicContentInlineSize();
  if (default_size != kIndefiniteSize) {
    ipid_depth_log.FPrint(
        "元素 {} 设置了 DefaultIntrinsicContentInlineSize = "
        "{}（例如 textarea 等），导致其固有宽度 = (border + 滚动条 + "
        "padding){} + "
        "DefaultIntrinsicContentInlineSize，不依赖子元素。",
        ipid_node_str, default_size, border_scrollbar_padding.InlineSum());
    sizes += default_size;
    // <textarea>'s intrinsic size should ignore scrollbar existence.
    if (node.IsTextArea()) {
      sizes -= ComputeScrollbarsForNonAnonymous(node).InlineSum();
    }
    return MinMaxSizesResult{sizes,
                             /* depends_on_block_constraints */ false};
  }

  // Size contained elements don't consider children for intrinsic sizing.
  // Also, if we don't have children, we can determine the size immediately.
  if (node.ShouldApplyInlineSizeContainment() || !node.FirstChild()) {
    ipid_depth_log.FPrint(
        "元素 {} 根本没有子元素，或者其设置了 contain: size，"
        "导致其固有宽度 = (border + 滚动条 + padding){}，不依赖子元素。",
        ipid_node_str, border_scrollbar_padding.InlineSum());
    return MinMaxSizesResult{sizes,
                             /* depends_on_block_constraints */ false};
  }

  ipid_depth_log.FPrint("元素 {} 的固有宽度不能独立计算，必须依赖其子元素。",
                        ipid_node_str);
  return std::nullopt;
}

LayoutUnit CalculateIntrinsicBlockSizeIgnoringChildren(
    const BlockNode& node,
    const BoxStrut& border_scrollbar_padding,
    bool children_have_geometry) {
  // Check if the intrinsic size was overridden.
  const LayoutUnit override_size = node.OverrideIntrinsicContentBlockSize();
  if (override_size != kIndefiniteSize) {
    return override_size + border_scrollbar_padding.BlockSum();
  }

  // Check if we have a "default" size (a <textarea>).
  const LayoutUnit default_block_size =
      node.DefaultIntrinsicContentBlockSize(children_have_geometry);
  if (default_block_size != kIndefiniteSize) {
    // <textarea>'s intrinsic size should ignore scrollbar existence.
    if (node.IsTextArea()) {
      return default_block_size -
             ComputeScrollbarsForNonAnonymous(node).BlockSum() +
             border_scrollbar_padding.BlockSum();
    }
    return default_block_size + border_scrollbar_padding.BlockSum();
  }

  if (node.ShouldApplyBlockSizeContainment()) {
    return border_scrollbar_padding.BlockSum();
  }

  return kIndefiniteSize;
}

void AddScrollbarFreeze(const BoxStrut& scrollbars_before,
                        const BoxStrut& scrollbars_after,
                        WritingDirectionMode writing_direction,
                        bool* freeze_horizontal,
                        bool* freeze_vertical) {
  PhysicalBoxStrut physical_before =
      scrollbars_before.ConvertToPhysical(writing_direction);
  PhysicalBoxStrut physical_after =
      scrollbars_after.ConvertToPhysical(writing_direction);
  *freeze_horizontal |= (!physical_before.top && physical_after.top) ||
                        (!physical_before.bottom && physical_after.bottom);
  *freeze_vertical |= (!physical_before.left && physical_after.left) ||
                      (!physical_before.right && physical_after.right);
}

}  // namespace blink
