#include "build/build_config.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "third_party/blink/renderer/core/dom/dom_token_list.h"
#include "third_party/blink/renderer/core/dom/node_computed_style.h"
#include "third_party/blink/renderer/core/dom/tag_collection.h"
#include "third_party/blink/renderer/core/layout/base_layout_algorithm_test.h"
#include "third_party/blink/renderer/core/layout/block_break_token.h"
#include "third_party/blink/renderer/core/layout/block_layout_algorithm.h"
#include "third_party/blink/renderer/core/layout/block_node.h"
#include "third_party/blink/renderer/core/layout/constraint_space.h"
#include "third_party/blink/renderer/core/layout/constraint_space_builder.h"
#include "third_party/blink/renderer/core/layout/flex/flexible_box_algorithm.h"
#include "third_party/blink/renderer/core/layout/flex/layout_flexible_box.h"
#include "third_party/blink/renderer/core/layout/grid/grid_layout_algorithm.h"
#include "third_party/blink/renderer/core/layout/layout_block_flow.h"
#include "third_party/blink/renderer/core/layout/layout_result.h"
#include "third_party/blink/renderer/core/layout/length_utils.h"
#include "third_party/blink/renderer/core/layout/min_max_sizes.h"
#include "third_party/blink/renderer/core/layout/physical_box_fragment.h"
#include "third_party/blink/renderer/core/layout/physical_fragment.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/core/testing/core_unit_test_helper.h"

namespace blink {
namespace {

class IpidTest : public BaseLayoutAlgorithmTest {
public:
  MinMaxSizes RunComputeMinMaxSizes(BlockNode node) {
    // The constraint space is not used for min/max computation, but we need
    // it to create the algorithm.
    ConstraintSpace space = ConstructBlockLayoutTestConstraintSpace(
        {WritingMode::kHorizontalTb, TextDirection::kLtr},
        LogicalSize(LayoutUnit(), LayoutUnit()));
    FragmentGeometry fragment_geometry = CalculateInitialFragmentGeometry(
        space, node, /* break_token */ nullptr, /* is_intrinsic */ true);

    BlockLayoutAlgorithm algorithm({node, fragment_geometry, space});
    return algorithm.ComputeMinMaxSizes(MinMaxSizesFloatInput()).sizes;
  }

  void PrintMinMaxSizesResult(const MinMaxSizesResult &result) {
    std::cout << "  sizes: " << result.sizes << std::endl;
    std::cout << "  depends_on_block_constraints: "
              << result.depends_on_block_constraints << std::endl;
    std::cout << "  applied_aspect_ratio: " << result.applied_aspect_ratio
              << std::endl;
  }
};

// TEST_F(IpidTest, Test1) {
//   SetBodyInnerHTML(R"HTML(
//     <div id="container" style="width: 30px; height: 40px;">
//       <div id="box" style="width: 10px; height: 20px;"></div>
//     </div>
//   )HTML");

//   BlockNode node_container(
//       To<LayoutBlockFlow>(GetLayoutObjectByElementId("container")));

//   MinMaxSizes sizes;
//   sizes = RunComputeMinMaxSizes(node_container);
//   std::cout << "#container 固有尺寸: " << sizes << std::endl;

//   BlockNode node_box(To<LayoutBlockFlow>(GetLayoutObjectByElementId("box")));

//   sizes = RunComputeMinMaxSizes(node_box);
//   std::cout << "#box 固有尺寸: " << sizes << std::endl;
// }

// TEST_F(IpidTest, Test2) {
//   SetBodyInnerHTML(R"HTML(
//     <style>
//       #a {
//         height: 100px;
//         width: min-content;
//         background: #aaaaaa;
//       }

//       #b {
//         display: flex;
//         height: 90%;
//         background: #bbbbbb;
//       }

//       #c {
//         aspect-ratio: 1 / 1;
//         background: #cccccc;
//       }
//     </style>

//     <div id="a">
//       <div id="b">
//         <div id="c"></div>
//       </div>
//     </div>
//   )HTML");

//   ConstraintSpace space = ConstructBlockLayoutTestConstraintSpace(
//       {WritingMode::kHorizontalTb, TextDirection::kLtr},
//       LogicalSize(LayoutUnit(500), LayoutUnit(500)));

//   LayoutBlockFlow *layout_a =
//       To<LayoutBlockFlow>(GetLayoutObjectByElementId("a"));
//   layout_a->SetIntrinsicLogicalWidthsDirty();

//   // BlockNode node_a(layout_a);
//   // const PhysicalBoxFragment *fragment = RunBlockLayoutAlgorithm(node_a,
//   // space);

//   // std::cout
//   //     << "\n布局结果：\n"
//   //     <<
//   // fragment->DumpFragmentTree(PhysicalFragment::DumpFlag::DumpAll).Utf8()
//   //     << std::endl;

//   MinMaxSizesResult sizes;

//   // sizes = node_a.ComputeMinMaxSizes(WritingMode::kHorizontalTb,
//   //                                   SizeType::kContent, space);
//   // std::cout << "#a 的 kContent 尺寸: \n";
//   // PrintMinMaxSizesResult(sizes);
//   // std::cout << std::endl;

//   // sizes = node_a.ComputeMinMaxSizes(WritingMode::kHorizontalTb,
//   //                                   SizeType::kIntrinsic, space);
//   // std::cout << "#a 的 kIntrinsic 尺寸: \n";
//   // PrintMinMaxSizesResult(sizes);
//   // std::cout << std::endl;

//   BlockNode node_b(To<LayoutFlexibleBox>(GetLayoutObjectByElementId("b")));

//   // sizes = node_b.ComputeMinMaxSizes(WritingMode::kHorizontalTb,
//   //                                   SizeType::kContent, space);
//   // std::cout << "#b 的 kContent 尺寸: \n";
//   // PrintMinMaxSizesResult(sizes);
//   // std::cout << std::endl;

//   node_b.GetLayoutBox()->SetIntrinsicLogicalWidthsDirty();
//   sizes = node_b.ComputeMinMaxSizes(WritingMode::kHorizontalTb,
//                                     SizeType::kIntrinsic, space);
//   std::cout << "#b 的 kIntrinsic 尺寸: \n";
//   PrintMinMaxSizesResult(sizes);
//   std::cout << std::endl;

//   // BlockNode node_c(To<LayoutBlockFlow>(GetLayoutObjectByElementId("c")));

//   // sizes = node_c.ComputeMinMaxSizes(WritingMode::kHorizontalTb,
//   //                                   SizeType::kContent, space);
//   // std::cout << "#c 的 kContent 尺寸: \n";
//   // PrintMinMaxSizesResult(sizes);
//   // std::cout << std::endl;

//   // sizes = node_c.ComputeMinMaxSizes(WritingMode::kHorizontalTb,
//   //                                   SizeType::kIntrinsic, space);
//   // std::cout << "#c 的 kIntrinsic 尺寸: \n";
//   // PrintMinMaxSizesResult(sizes);
//   // std::cout << std::endl;
// }

TEST_F(IpidTest, Test3) {
  SetBodyInnerHTML(R"HTML(
    <style>
      #a {
        height: 107px;
        width: min-content;
        background: #aaaaaa;
      }

      #b {
        display: flex;
        height: 89%;
        background: #bbbbbb;
      }

      #c {
        aspect-ratio: 5 / 7;
        background: #cccccc;
      }
    </style>

    <div id="a">
      <div id="b">
        <div id="c"></div>
      </div>
    </div>
  )HTML");

  ConstraintSpace space = ConstructBlockLayoutTestConstraintSpace(
      {WritingMode::kHorizontalTb, TextDirection::kLtr},
      LogicalSize(LayoutUnit(499), LayoutUnit(503)));

  std::cout << "\n[ipid] before layout" << std::endl;

  RunBlockLayoutAlgorithm(
      BlockNode(To<LayoutBox>(GetLayoutObjectByElementId("a"))), space);

  std::cout << "\n[ipid] after layout" << std::endl;
}

} // namespace
} // namespace blink
