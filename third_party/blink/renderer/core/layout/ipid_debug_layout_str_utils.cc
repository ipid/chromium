#include "third_party/blink/renderer/core/layout/ipid_debug_layout_str_utils.h"

#include <sstream>

#include "third_party/blink/renderer/core/layout/geometry/logical_size.h"
#include "third_party/blink/renderer/core/layout/layout_result.h"
#include "third_party/blink/renderer/core/layout/length_utils.h"
#include "third_party/blink/renderer/core/layout/min_max_sizes.h"

namespace blink::ipid {

const char* btos(const bool b) {
  return b ? "true" : "false";
}

// 返回SizeType的字符串表示
const char* GetSizeTypeString(const SizeType type) {
  switch (type) {
    case SizeType::kContent:
      return "kContent";
    case SizeType::kIntrinsic:
      return "kIntrinsic";
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

  oss << "MinMaxSizesResult{\n  sizes = " << result.sizes
      << ",\n  depends_on_block_constraints = "
      << result.depends_on_block_constraints
      << ",\n  applied_aspect_ratio = " << result.applied_aspect_ratio << "\n}";

  return oss.str();
}

std::string GetAspectRatioString(const LogicalSize& aspect_ratio) {
  std::ostringstream oss;
  oss << aspect_ratio.inline_size << " / " << aspect_ratio.block_size;
  return oss.str();
}

std::string GetLayoutResultString(const LayoutResult* layout_result) {
  std::ostringstream oss;

  oss << "LayoutResult {\n  status = "
      << GetLayoutResultStatusString(layout_result->Status())
      << ",\n  fragment =\n"
      << layout_result->GetPhysicalFragment()
             .DumpFragmentTree(PhysicalFragment::DumpFlag::DumpAll)
             .Utf8()
      << "}";

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
      << "\n  bfc_offset = ("
      << constraint_space.GetBfcOffset().ToString().Utf8()
      << "),\n  available_size = (" << constraint_space.AvailableSize()
      << "),\n  percentage_resolution_size = ("
      << constraint_space.PercentageResolutionSize()
      << "),\n  inline_auto_behavior = "
      << GetAutoSizeBehaviorString(constraint_space.InlineAutoBehavior())
      << ",\n  block_auto_behavior = "
      << GetAutoSizeBehaviorString(constraint_space.BlockAutoBehavior())
      << ",\n}";

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
      return "kAsSpecified";
    case CalcSizeKeywordBehavior::kAsAuto:
      return "kAsAuto";
    default:
      return "<Unknown>";
  }
}

std::string GetLogicalSizeString(const LogicalSize& logical_size) {
  std::ostringstream oss;
  oss << "Logical {" << logical_size.inline_size << "x" << logical_size.block_size << "}";
  return oss.str();
}

}  // namespace blink::ipid
