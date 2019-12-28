add_library(zhpe_offloaded_stats INTERFACE)
if (ZSTA)
  set(ZSTA_LIB ${INSD}/lib/libzhpe_offloaded_stats.so)
  target_link_libraries(zhpe_offloaded_stats INTERFACE ${ZSTA_LIB})
  message("ZSTA_LIB " ${ZSTA_LIB})
  target_compile_definitions(zhpe_offloaded_stats INTERFACE -DHAVE_ZHPE_STATS)
endif(ZSTA)

