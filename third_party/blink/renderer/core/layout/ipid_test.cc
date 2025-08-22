#include <fstream>
#include <iostream>
#include <sstream>

#include "gtest/gtest.h"
#include "third_party/blink/renderer/core/layout/base_layout_algorithm_test.h"
#include "third_party/blink/renderer/platform/ipid_logging/ipid_depth_logging.h"

namespace blink {
namespace {

class IpidTest : public BaseLayoutAlgorithmTest {
 protected:
  void SetUp() override {
    BaseLayoutAlgorithmTest::SetUp();
    LoadAhem();
  }
};

TEST_F(IpidTest, TestDynamicHTML) {
  ASSERT_TRUE(GetDocument().InNoQuirksMode());

  std::ifstream test_body_file("./test-body.html");
  std::stringstream buffer;

  if (test_body_file.is_open()) {
    buffer << test_body_file.rdbuf();
    test_body_file.close();
  } else {
    std::cerr << "[ipid] 无法打开文件: ./test-body.html" << std::endl;
    std::abort();
  }

  std::string html_content = buffer.str();

  std::cout
      << "\n[ipid] --------------- [BEGIN READ TEST BODY] ---------------\n";
  SetIpidLoggingEnabled(true);
  SetBodyInnerHTML(WTF::String(html_content));
  SetIpidLoggingEnabled(false);
  std::cout << "\n[ipid] --------------- [END READ TEST BODY] ---------------"
            << std::endl;
}

} // namespace
} // namespace blink
