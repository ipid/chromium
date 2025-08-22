// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/layout/flex/line_flexer.h"

// ------ ipid logging START ------
#include "third_party/blink/renderer/core/layout/ipid_debug_layout_str_utils.h"
#include "third_party/blink/renderer/platform/ipid_logging/ipid_depth_logging.h"
// ------ ipid logging END ------

namespace blink {

LineFlexer::LineFlexer(base::span<FlexItem> line_items,
                       LayoutUnit sum_hypothetical_main_size,
                       LayoutUnit sum_flex_base_size,
                       LayoutUnit main_axis_inner_size)
    : line_items_(line_items),
      flex_sign_(sum_hypothetical_main_size < main_axis_inner_size
                     ? kPositive
                     : kNegative) {
  // Per https://drafts.csswg.org/css-flexbox/#resolve-flexible-lengths step 2,
  // we freeze all items with a flex factor of 0 as well as those with a min/max
  // size violation.
  remaining_free_space_ = main_axis_inner_size - sum_flex_base_size;

  ViolationsVector new_inflexible_items;
  for (auto& flex_item : line_items_) {
    DCHECK(!flex_item.frozen);

    total_flex_grow_ += flex_item.flex_grow;
    total_flex_shrink_ += flex_item.flex_shrink;
    total_weighted_flex_shrink_ +=
        flex_item.flex_shrink * flex_item.base_content_size;

    float flex_factor =
        (flex_sign_ == kPositive) ? flex_item.flex_grow : flex_item.flex_shrink;
    if (flex_factor == 0 ||
        (flex_sign_ == kPositive &&
         flex_item.base_content_size > flex_item.hypothetical_content_size) ||
        (flex_sign_ == kNegative &&
         flex_item.base_content_size < flex_item.hypothetical_content_size)) {
      flex_item.flexed_content_size = flex_item.hypothetical_content_size;
      new_inflexible_items.push_back(&flex_item);
    }
  }
  FreezeViolations(new_inflexible_items);
  initial_free_space_ = remaining_free_space_;
}

void LineFlexer::FreezeViolations(ViolationsVector& violations) {
  for (auto* violation : violations) {
    DCHECK(!violation->frozen);
    remaining_free_space_ -=
        violation->flexed_content_size - violation->base_content_size;
    total_flex_grow_ -= violation->flex_grow;
    total_flex_shrink_ -= violation->flex_shrink;
    total_weighted_flex_shrink_ -=
        violation->flex_shrink * violation->base_content_size;
    // total_weighted_flex_shrink can be negative when we exceed the precision
    // of a double when we initially calculate total_weighted_flex_shrink. We
    // then subtract each child's weighted flex shrink with full precision, now
    // leading to a negative result. See
    // css3/flexbox/large-flex-shrink-assert.html
    total_weighted_flex_shrink_ = std::max(total_weighted_flex_shrink_, 0.0);
    violation->frozen = true;
  }
}

bool LineFlexer::ResolveFlexibleLengths() {
  IpidDepthLog ipid_depth_log(
      "line_flexer.cc: LineFlexer::ResolveFlexibleLengths");

  ipid_depth_log.FPrint(
      "开始进行 CSS Flexbox 弹性长度解析的一轮迭代。根据 CSS Flexbox "
      "规范（https://drafts.csswg.org/css-flexbox/#resolve-flexible-lengths），"
      "此函数会按以下步骤进行：\n"
      "1. 计算每个未冻结项目的目标主轴尺寸\n"
      "2. 检查是否违反 min/max 约束\n"
      "3. 如果有违反，冻结违反项目并重新分配剩余空间\n"
      "4. 如果没有违反，算法收敛，返回 false\n"
      "\n当前状态：\n"
      "- 当前 flex_sign_: {} （决定是扩展还是收缩）\n"
      "- 剩余自由空间: {}px\n"
      "- 总 flex-grow: {}\n"
      "- 总 flex-shrink: {}\n"
      "- 总加权 flex-shrink: {}",
      ipid::GetFlexSignString(static_cast<int>(flex_sign_)),
      remaining_free_space_, total_flex_grow_, total_flex_shrink_,
      total_weighted_flex_shrink_);

  LayoutUnit total_violation;
  LayoutUnit used_free_space;
  ViolationsVector min_violations;
  ViolationsVector max_violations;

  const double sum_flex_factors =
      (flex_sign_ == kPositive) ? total_flex_grow_ : total_flex_shrink_;

  ipid_depth_log.FPrint(
      "步骤1: 处理 flex 因子总和小于 1 的特殊情况。当前 flex 因子总和为 {}。",
      sum_flex_factors);

  if (sum_flex_factors > 0 && sum_flex_factors < 1) {
    LayoutUnit fractional(initial_free_space_ * sum_flex_factors);
    ipid_depth_log.FPrint(
        "特殊情况：flex 因子总和 {} 在 (0,1) 范围内，根据 CSS Flexbox "
        "规范，需要限制使用的自由空间。\n"
        "计算按比例分配的空间：{} * {} = {}px\n"
        "当前剩余自由空间：{}px",
        sum_flex_factors, initial_free_space_, sum_flex_factors, fractional,
        remaining_free_space_);

    if (fractional.Abs() < remaining_free_space_.Abs()) {
      remaining_free_space_ = fractional;
      ipid_depth_log.FPrint(
          "由于按比例分配的空间绝对值（{}px）小于剩余自由空间绝对值（{}px），"
          "将剩余自由空间限制为 {}px，以避免过度伸缩。",
          fractional.Abs(), remaining_free_space_.Abs(), fractional);
    } else {
      ipid_depth_log.FPrint(
          "按比例分配的空间绝对值（{}px）不小于剩余自由空间绝对值（{}px），"
          "保持剩余自由空间不变。",
          fractional.Abs(), remaining_free_space_.Abs());
    }
  }

  ipid_depth_log.FPrint(
      "步骤2: 遍历所有 flex item，为每个未冻结的项目计算目标主轴尺寸。\n"
      "当前剩余自由空间：{}px",
      remaining_free_space_);

  for (auto& flex_item : line_items_) {
    if (flex_item.frozen) {
      continue;
    }

    ipid_depth_log.FPrint("处理 flex item:\n{}",
                          ipid::GetFlexItemString(flex_item));

    LayoutUnit child_size = flex_item.base_content_size;
    double extra_space = 0;

    ipid_depth_log.FPrint(
        "开始计算该项目的额外空间分配。初始尺寸为 base_content_size = {}px。",
        child_size);

    if (remaining_free_space_ > 0 && total_flex_grow_ > 0 &&
        flex_sign_ == kPositive && std::isfinite(total_flex_grow_)) {
      extra_space =
          remaining_free_space_ * flex_item.flex_grow / total_flex_grow_;

      ipid_depth_log.FPrint(
          "【扩展模式】剩余自由空间为正值 {}px，进行 flex-grow 计算：\n"
          "额外空间 = 剩余自由空间 * 项目flex-grow / 总flex-grow\n"
          "         = {} * {} / {} = {}px\n"
          "该项目将从 {}px 扩展到 {}px。",
          remaining_free_space_, remaining_free_space_, flex_item.flex_grow,
          total_flex_grow_, extra_space, child_size,
          child_size + LayoutUnit::FromFloatRound(extra_space));

    } else if (remaining_free_space_ < 0 && total_weighted_flex_shrink_ > 0 &&
               flex_sign_ == kNegative &&
               std::isfinite(total_weighted_flex_shrink_) &&
               flex_item.flex_shrink) {
      extra_space = remaining_free_space_ * flex_item.flex_shrink *
                    flex_item.base_content_size / total_weighted_flex_shrink_;

      ipid_depth_log.FPrint(
          "【收缩模式】剩余自由空间为负值 {}px，进行 flex-shrink 计算：\n"
          "额外空间 = 剩余自由空间 * 项目flex-shrink * 项目base_content_size / "
          "总加权flex-shrink\n"
          "         = {} * {} * {} / {} = {}px\n"
          "该项目将从 {}px 收缩到 {}px。",
          remaining_free_space_, remaining_free_space_, flex_item.flex_shrink,
          flex_item.base_content_size, total_weighted_flex_shrink_, extra_space,
          child_size, child_size + LayoutUnit::FromFloatRound(extra_space));

    } else {
      ipid_depth_log.FPrint(
          "不满足 flex-grow 或 flex-shrink 的条件，该项目保持原始尺寸 {}px。\n"
          "条件检查：\n"
          "- 剩余自由空间: {}px (>0 才能扩展, <0 才能收缩)\n"
          "- total_flex_grow_: {} (需要>0)\n"
          "- total_weighted_flex_shrink_: {} (需要>0)\n"
          "- flex_sign_: {}\n"
          "- 项目 flex_shrink: {} (需要>0)",
          child_size, remaining_free_space_, total_flex_grow_,
          total_weighted_flex_shrink_,
          ipid::GetFlexSignString(static_cast<int>(flex_sign_)),
          flex_item.flex_shrink);
    }

    if (std::isfinite(extra_space)) {
      child_size += LayoutUnit::FromFloatRound(extra_space);
      ipid_depth_log.FPrint(
          "额外空间 {}px 是有限值，将其加到初始尺寸上：{} + {} = {}px。",
          extra_space, flex_item.base_content_size,
          LayoutUnit::FromFloatRound(extra_space), child_size);
    } else {
      ipid_depth_log.FPrint(
          "额外空间 {} 不是有限值（可能为无穷大或NaN），保持尺寸为 {}px。",
          extra_space, child_size);
    }

    const LayoutUnit adjusted_child_size =
        flex_item.main_axis_min_max_sizes.ClampSizeToMinAndMax(child_size);

    ipid_depth_log.FPrint(
        "步骤3: 应用 min/max 约束。\n"
        "计算得到的尺寸：{}px\n"
        "min/max 约束：{}\n"
        "约束后的尺寸：{}px",
        child_size,
        ipid::GetMinMaxSizesString(flex_item.main_axis_min_max_sizes),
        adjusted_child_size);

    DCHECK_GE(adjusted_child_size, 0);
    flex_item.flexed_content_size = adjusted_child_size;

    const LayoutUnit used_space_by_this_item =
        adjusted_child_size - flex_item.base_content_size;
    used_free_space += used_space_by_this_item;

    ipid_depth_log.FPrint(
        "该项目使用的自由空间：{} - {} = {}px\n"
        "累计已使用的自由空间：{}px",
        adjusted_child_size, flex_item.base_content_size,
        used_space_by_this_item, used_free_space);

    const LayoutUnit violation = adjusted_child_size - child_size;

    if (violation > 0) {
      min_violations.push_back(&flex_item);
      ipid_depth_log.FPrint(
          "步骤4: 检测到 min 约束违反。约束后尺寸（{}px）大于计算尺寸（{}px），"
          "违反量为 {}px。该项目将被标记为 min_violation。",
          adjusted_child_size, child_size, violation);
    } else if (violation < 0) {
      max_violations.push_back(&flex_item);
      ipid_depth_log.FPrint(
          "步骤4: 检测到 max 约束违反。约束后尺寸（{}px）小于计算尺寸（{}px），"
          "违反量为 {}px。该项目将被标记为 max_violation。",
          adjusted_child_size, child_size, violation);
    } else {
      ipid_depth_log.FPrint(
          "步骤4: 无约束违反。约束后尺寸（{}px）等于计算尺寸（{}px）。",
          adjusted_child_size, child_size);
    }

    total_violation += violation;

    ipid_depth_log.FPrint(
        "当前总违反量：{}px（正值表示 min 违反，负值表示 max 违反）",
        total_violation);
  }

  ipid_depth_log.FPrint(
      "步骤5: 处理约束违反的结果。\n"
      "总违反量：{}px\n"
      "min_violations 数量：{}\n"
      "max_violations 数量：{}",
      total_violation, min_violations.size(), max_violations.size());

  if (total_violation) {
    auto& violations_to_freeze =
        total_violation < 0 ? max_violations : min_violations;
    ipid_depth_log.FPrint(
        "存在约束违反，将冻结 {} 个项目（{}）并重新分配剩余空间。\n"
        "下一轮迭代将继续处理剩余未冻结的项目。",
        violations_to_freeze.size(),
        total_violation < 0 ? "max_violations" : "min_violations");

    FreezeViolations(violations_to_freeze);
  } else {
    remaining_free_space_ -= used_free_space;
    ipid_depth_log.FPrint(
        "无约束违反，算法收敛。\n"
        "从剩余自由空间中减去已使用空间：{} - {} = {}px\n"
        "弹性长度解析完成，返回 false 表示算法结束。",
        remaining_free_space_ + used_free_space, used_free_space,
        remaining_free_space_);
  }

  ipid_depth_log.FPrint(
      "本轮迭代结束，返回 {}（true 表示需要继续迭代，false 表示算法收敛）。",
      ipid::btos(static_cast<bool>(total_violation)));

  return total_violation;
}

}  // namespace blink
