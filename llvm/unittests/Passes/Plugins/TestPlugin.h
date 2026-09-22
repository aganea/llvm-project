#define TEST_PLUGIN_NAME "TestPlugin"
#define TEST_PLUGIN_VERSION "0.1-unit"
#define TEST_PLUGIN_OPTION_NAME "test-plugin-dynamic-option"

#define TEST_PLUGIN_OPTION_VALUE_SYMBOL "testPluginDynamicOptionValue"
#define TEST_PLUGIN_OPTION_ADDRESS_SYMBOL "testPluginDynamicOptionAddress"
#define TEST_PLUGIN_CONSTRUCTION_COUNT_SYMBOL                                  \
  "testPluginDynamicOptionConstructionCount"
#define TEST_PLUGIN_DESTRUCTION_COUNT_SYMBOL                                   \
  "testPluginDynamicOptionDestructionCount"
#define TEST_PLUGIN_POST_OPTION_DESTRUCTION_COUNT_SYMBOL                       \
  "testPluginDynamicOptionPostOptionDestructionCount"

using TestPluginOptionValueFn = const char *(*)();
using TestPluginOptionAddressFn = const void *(*)();
using TestPluginOptionCountFn = unsigned (*)();
