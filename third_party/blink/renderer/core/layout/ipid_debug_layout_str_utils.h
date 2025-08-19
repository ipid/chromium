#include <stdint.h>

#include <string>

#include "third_party/blink/renderer/core/core_export.h"

namespace blink {

enum class SizeType;
struct MinMaxSizes;
struct MinMaxSizesResult;
struct LogicalSize;
class LayoutResult;
enum class AutoSizeBehavior : uint8_t;
enum class LengthTypeInternal;
enum class FitContentMode;
enum class CalcSizeKeywordBehavior;
class ConstraintSpace;

namespace ipid {

CORE_EXPORT const char* btos(const bool b);
CORE_EXPORT const char* GetSizeTypeString(const SizeType type);
CORE_EXPORT const char* GetLayoutResultStatusString(const int status);
CORE_EXPORT std::string GetMinMaxSizesString(const MinMaxSizes& sizes);
CORE_EXPORT std::string GetMinMaxSizesResultString(
    const MinMaxSizesResult& result);
CORE_EXPORT std::string GetAspectRatioString(const LogicalSize& aspect_ratio);
CORE_EXPORT std::string GetLayoutResultString(
    const LayoutResult* layout_result);
CORE_EXPORT const char* GetAutoSizeBehaviorString(
    const AutoSizeBehavior& behavior);
CORE_EXPORT const char* GetLengthTypeInternalString(
    const LengthTypeInternal& type);
CORE_EXPORT const char* GetFitContentModeString(const FitContentMode& mode);
CORE_EXPORT const char* GetCalcSizeKeywordBehaviorString(
    const CalcSizeKeywordBehavior& behavior);
CORE_EXPORT std::string GetConstraintSpaceString(
    const ConstraintSpace& constraint_space);
CORE_EXPORT std::string GetLogicalSizeString(const LogicalSize& logical_size);

}  // namespace ipid

}  // namespace blink
