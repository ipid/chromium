#include "third_party/blink/renderer/core/layout/layout_result.h"
#include "third_party/blink/renderer/core/layout/length_utils.h"

namespace blink::ipid {

CORE_EXPORT const char* btos(bool b);
CORE_EXPORT const char* GetSizeTypeString(SizeType type);
CORE_EXPORT const char* GetLayoutResultStatusString(LayoutResult::EStatus status);
CORE_EXPORT std::string GetMinMaxSizesString(const MinMaxSizes& sizes);
CORE_EXPORT std::string GetMinMaxSizesResultString(const MinMaxSizesResult& result);
CORE_EXPORT std::string GetAspectRatioString(const LogicalSize& aspect_ratio);
CORE_EXPORT std::string GetLayoutResultString(const LayoutResult* layout_result);
CORE_EXPORT const char* GetAutoSizeBehaviorString(AutoSizeBehavior behavior);
CORE_EXPORT const char* GetLengthTypeInternalString(LengthTypeInternal type);
CORE_EXPORT const char* GetFitContentModeString(FitContentMode mode);
CORE_EXPORT const char* GetCalcSizeKeywordBehaviorString(CalcSizeKeywordBehavior behavior);
CORE_EXPORT std::string GetConstraintSpaceString(const ConstraintSpace& constraint_space);

}  // namespace blink::ipid
