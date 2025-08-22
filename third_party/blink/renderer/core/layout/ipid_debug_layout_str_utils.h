#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_LAYOUT_IPID_DEBUG_LAYOUT_STR_UTILS_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_LAYOUT_IPID_DEBUG_LAYOUT_STR_UTILS_H_

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
class ConstraintSpace;
struct BoxStrut;
enum class CalcSizeKeywordBehavior;
class LayoutInputNode;
class LayoutResult;
class LayoutBox;
class LayoutObject;
class PhysicalFragment;
struct FragmentGeometry;
enum class LayoutCacheStatus;
enum class LineBreakerMode;
struct MarginStrut;
enum class EFloat : uint8_t;
enum class EClear : uint8_t;
enum class ItemPosition : unsigned;
struct PhysicalBoxStrut;
enum class BaselineGroup;
struct FlexItemData;
struct FlexItem;
struct FlexLine;
struct FlexColumnBreakInfo;
enum class ContentDistributionType : unsigned;
enum class ContentPosition : unsigned;
enum class OverflowAlignment : unsigned;
class StyleContentAlignmentData;

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
CORE_EXPORT std::string GetBoxStrutString(const BoxStrut& box_strut);
CORE_EXPORT std::string GetBoxStrutString(const PhysicalBoxStrut& box_strut);
CORE_EXPORT const char* GetLengthTypeString(const unsigned char type);
CORE_EXPORT std::string GetNodeStr(const LayoutInputNode& block_node);
CORE_EXPORT std::string GetNodeStr(const LayoutObject* layout_object);
CORE_EXPORT std::string GetNodeStr(const LayoutResult* layout_result);
CORE_EXPORT std::string GetNodeStr(const PhysicalFragment& physical_fragment);
CORE_EXPORT std::string GetFragmentGeometryString(
    const FragmentGeometry& fragment_geometry);
CORE_EXPORT const char* GetLayoutCacheStatusString(
    const LayoutCacheStatus& status);
CORE_EXPORT const char* GetLineBreakerModeString(const LineBreakerMode& mode);
CORE_EXPORT std::string GetMarginStrutString(const MarginStrut& margin_strut);
CORE_EXPORT const char* GetComputedStyleEFloatStr(const EFloat& e_float);
CORE_EXPORT const char* GetComputedStyleEClearStr(const EClear& e_clear);
CORE_EXPORT const char* GetItemPositionString(
    const ItemPosition& item_position);
CORE_EXPORT const char* GetFlexLayoutAlgorithmPhaseString(int phase);
CORE_EXPORT const char* GetBaselineGroupString(
    const BaselineGroup& baseline_group);
CORE_EXPORT const char* GetFlexBreakBeforeRowString(int break_before_row);
CORE_EXPORT const char* GetBreakStatusString(int break_status);
CORE_EXPORT std::string GetFlexItemDataString(
    const FlexItemData& flex_item_data);
CORE_EXPORT const char* GetFlexSignString(int flex_sign);
CORE_EXPORT std::string GetFlexItemString(const FlexItem& flex_item);
CORE_EXPORT std::string GetFlexLineString(const FlexLine& flex_line);
CORE_EXPORT std::string GetFlexColumnBreakInfoString(
    const FlexColumnBreakInfo* break_info);
CORE_EXPORT const char* GetContentDistributionTypeString(
    const ContentDistributionType& distribution_type);
CORE_EXPORT const char* GetContentPositionString(
    const ContentPosition& content_position);
CORE_EXPORT const char* GetOverflowAlignmentString(
    const OverflowAlignment& overflow_alignment);
CORE_EXPORT std::string GetStyleContentAlignmentDataString(
    const StyleContentAlignmentData& alignment_data);

}  // namespace ipid

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_LAYOUT_IPID_DEBUG_LAYOUT_STR_UTILS_H_
