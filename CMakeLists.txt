cmake_minimum_required(VERSION 3.21)
project(DabanQuant VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/utf-8 /W3 /permissive- /Zc:__cplusplus /bigobj)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------- Qt
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Sql PrintSupport)

qt_standard_project_setup()

# ------------------------------------------------- QCustomPlot (static)
add_library(qcustomplot STATIC
    third_party/qcustomplot/qcustomplot.cpp
    third_party/qcustomplot/qcustomplot.h
)
target_include_directories(qcustomplot PUBLIC third_party/qcustomplot)
target_link_libraries(qcustomplot PUBLIC Qt6::Widgets Qt6::PrintSupport)
set_target_properties(qcustomplot PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------- 公共新增模块
# 零 GUI 依赖：GUI 与自检目标共用
set(COMMON_EXTRA_SOURCES
    src/core/Diagnostics.cpp
    src/core/SingleInstance.cpp
    src/core/TradingCalendar.cpp
    src/notify/CredentialStore.cpp
    src/strategy/FactorConfig.cpp
    src/strategy/MultiFactorEngine.cpp
    src/analysis/TradeAdvisor.cpp
    src/analysis/AiSettings.cpp
    src/analysis/AiAnalysisSpec.cpp
    src/analysis/AiAnalysisClient.cpp
    src/core/LegalNotice.cpp
    src/review/DecisionReview.cpp
)
set(COMMON_EXTRA_HEADERS
    src/core/Diagnostics.h
    src/core/SingleInstance.h
    src/core/TradingCalendar.h
    src/core/LegalNotice.h
    src/core/FactorTypes.h
    src/notify/CredentialStore.h
    src/strategy/FactorConfig.h
    src/strategy/MultiFactorEngine.h
    src/analysis/TradeAdvisor.h
    src/analysis/AiSettings.h
    src/analysis/AiAnalysisSpec.h
    src/analysis/AiAnalysisClient.h
    src/review/DecisionReview.h
)

# ---------------------------------------------------------------- MySQL 客户端库
# 交易日志需要落到 MySQL。Qt6 官方包未随附 qsqlmysql 插件，且本机也未装
# MySQL ODBC 驱动，因此直接使用 MySQL Server 自带的 Connector/C（libmysql）。
# 注意：这里**只取头文件路径**，不做静态链接 —— 静态导入 libmysql.dll 的可执行
# 文件会被本机设备保护（WDAC/AppLocker）策略在启动时拦截。
# 客户端库由 src/storage/MySqlStore.cpp 在运行时用 LoadLibrary 动态加载。
#
# 开关：关掉后交易日志只走本地 SQLite（离线环境 / 无 MySQL 客户端库时使用）。
option(DABAN_ENABLE_MYSQL_JOURNAL "交易日志写入 MySQL（需 MySQL Connector/C 头文件）" ON)

set(MYSQL_ROOT "")
file(GLOB _mysql_dirs "C:/Program Files/MySQL/MySQL Server */lib")
foreach(_d ${_mysql_dirs})
    if(EXISTS "${_d}/libmysql.lib")
        get_filename_component(_mysql_root "${_d}" DIRECTORY)
    endif()
endforeach()
if(_mysql_root)
    find_path(MYSQL_INCLUDE_DIR mysql.h HINTS "${_mysql_root}/include" NO_DEFAULT_PATH)
    file(GLOB MYSQL_CLIENT_DLL "${_mysql_root}/lib/libmysql.dll")
    message(STATUS "MySQL Connector/C headers: ${MYSQL_INCLUDE_DIR}")
endif()

# ---------------------------------------------------------------- app
set(SOURCES
    src/main.cpp
    ${COMMON_EXTRA_SOURCES}

    src/core/Indicators.cpp
    src/core/ChipDistribution.cpp
    src/core/TradePlan.cpp
    src/core/Auction.cpp
    src/core/RiskFlags.cpp

    src/data/TextCodec.cpp
    src/data/HttpClient.cpp
    src/data/LimitPoolSource.cpp
    src/data/KlineSource.cpp
    src/data/SnapshotSource.cpp
    src/data/ConceptSource.cpp
    src/data/StockDetailSource.cpp
    src/data/SectorFlowSource.cpp
    src/data/DataHub.cpp

    src/strategy/MarketRegime.cpp
    src/strategy/FilterPipeline.cpp
    src/strategy/Scorer.cpp
    src/strategy/RecommendEngine.cpp

    src/sentiment/SentimentEngine.cpp
    src/sentiment/LongSignalEngine.cpp

    src/notify/NotifyConfig.cpp
    src/notify/SmtpClient.cpp
    src/notify/PushService.cpp

    src/review/ReviewEngine.cpp
    src/review/ReviewAssistant.cpp

    src/storage/Database.cpp

    src/ui/Theme.cpp
    src/ui/Components.cpp
    src/ui/DisclaimerDialog.cpp
    src/ui/QcpAdapter.cpp
    src/ui/TableEmptyHint.cpp
    src/ui/MainWindow.cpp
    src/ui/ReviewPanel.cpp
    src/ui/JournalPanel.cpp
    src/ui/AiStockPanel.cpp
    src/ui/StockCard.cpp
    src/ui/KlineThumbnail.cpp
    src/ui/ChartPanel.cpp
    src/tests/CoreChecks.cpp
)

set(HEADERS
    ${COMMON_EXTRA_HEADERS}
    src/core/Types.h
    src/core/Indicators.h
    src/core/ChipDistribution.h
    src/core/TradePlan.h
    src/core/Auction.h
    src/core/RiskFlags.h
    src/data/TextCodec.h
    src/data/HttpClient.h
    src/data/LimitPoolSource.h
    src/data/KlineSource.h
    src/data/SnapshotSource.h
    src/data/ConceptSource.h
    src/data/StockDetailTypes.h
    src/data/StockDetailSource.h
    src/data/SectorFlowSource.h
    src/data/DataHub.h
    src/strategy/MarketRegime.h
    src/strategy/FilterPipeline.h
    src/strategy/Scorer.h
    src/strategy/RecommendEngine.h
    src/sentiment/SentimentTypes.h
    src/sentiment/SentimentEngine.h
    src/sentiment/LongSignalEngine.h
    src/notify/NotifyConfig.h
    src/notify/SmtpClient.h
    src/notify/PushService.h
    src/review/ReviewTypes.h
    src/review/ReviewEngine.h
    src/review/ReviewAssistant.h
    src/storage/Database.h
    src/storage/JournalStore.h
    src/storage/TradeRecord.h
    src/storage/MySqlStore.h
    src/ui/Theme.h
    src/ui/Components.h
    src/ui/DisclaimerDialog.h
    src/ui/QcpAdapter.h
    src/ui/TableEmptyHint.h
    src/ui/PlanCellDelegate.h
    src/ui/MainWindow.h
    src/ui/ReviewPanel.h
    src/ui/JournalPanel.h
    src/ui/AiStockPanel.h
    src/ui/StockCard.h
    src/ui/KlineThumbnail.h
    src/ui/ChartPanel.h
    src/tests/CoreChecks.h
)

add_executable(daban_quant WIN32 ${SOURCES} ${HEADERS})
target_link_libraries(daban_quant PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Sql Qt6::PrintSupport
    qcustomplot
)
target_include_directories(daban_quant PRIVATE src)
if(WIN32)
    target_link_libraries(daban_quant PRIVATE crypt32 dbghelp)
endif()

# 交易日志 MySQL 后端：只需要 Connector/C 的头文件（函数在运行时动态加载）
if(DABAN_ENABLE_MYSQL_JOURNAL AND MYSQL_INCLUDE_DIR)
    target_sources(daban_quant PRIVATE src/storage/MySqlStore.cpp)
    target_include_directories(daban_quant PRIVATE ${MYSQL_INCLUDE_DIR})
    target_compile_definitions(daban_quant PRIVATE DABAN_ENABLE_MYSQL_JOURNAL=1)
    # libmysql.dll 必须与 exe 放在同一目录：
    # 受限终端的设备保护策略允许从程序目录加载配套 DLL，
    # 但从 MySQL 安装目录直接加载会被判定违规并直接终止进程。
    if(MYSQL_CLIENT_DLL)
        add_custom_command(TARGET daban_quant POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MYSQL_CLIENT_DLL}" "$<TARGET_FILE_DIR:daban_quant>"
            COMMENT "复制 MySQL 客户端库 libmysql.dll 到程序目录")
    endif()
else()
    message(WARNING "MySQL 交易日志后端未启用：交易日志将只写本地 SQLite")
endif()

set_target_properties(daban_quant PROPERTIES
    WIN32_EXECUTABLE TRUE
    OUTPUT_NAME "DabanQuant"
)

# 自检版本：QCoreApplication，不链接 Widgets
set(SELFTEST_SOURCES
    src/selftest_main.cpp
    ${COMMON_EXTRA_SOURCES}
    src/core/Indicators.cpp
    src/core/ChipDistribution.cpp
    src/core/TradePlan.cpp
    src/core/Auction.cpp
    src/core/RiskFlags.cpp
    src/data/TextCodec.cpp
    src/data/HttpClient.cpp
    src/data/LimitPoolSource.cpp
    src/data/KlineSource.cpp
    src/data/SnapshotSource.cpp
    src/data/ConceptSource.cpp
    src/data/StockDetailSource.cpp
    src/data/StockDetailSource.h
    src/data/StockDetailTypes.h
    src/data/DataHub.cpp
    src/strategy/MarketRegime.cpp
    src/strategy/FilterPipeline.cpp
    src/strategy/Scorer.cpp
    src/strategy/RecommendEngine.cpp
    src/sentiment/SentimentEngine.cpp
    src/sentiment/LongSignalEngine.cpp
    src/notify/NotifyConfig.cpp
    src/notify/SmtpClient.cpp
    src/notify/PushService.cpp
    src/backtest/BacktestRunner.h
    src/backtest/BacktestRunner.cpp
    src/review/ReviewEngine.cpp
    src/review/ReviewTypes.h
    src/storage/Database.cpp
)
add_executable(daban_selftest ${SELFTEST_SOURCES})
target_include_directories(daban_selftest PRIVATE src)
target_link_libraries(daban_selftest PRIVATE Qt6::Core Qt6::Network Qt6::Sql)
if(WIN32)
    target_link_libraries(daban_selftest PRIVATE crypt32 dbghelp)
endif()
set_target_properties(daban_selftest PROPERTIES
    WIN32_EXECUTABLE FALSE
    OUTPUT_NAME "DabanQuantSelfTest"
)

# ---------------------------------------------------------------- 离线单元门禁
add_executable(daban_core_checks
    src/tests/core_checks_main.cpp
    src/tests/CoreChecks.cpp
    src/strategy/FactorConfig.cpp
    src/strategy/MultiFactorEngine.cpp
    src/analysis/TradeAdvisor.cpp
    src/analysis/AiAnalysisSpec.cpp
    src/analysis/AiSettings.cpp
    src/notify/CredentialStore.cpp
    src/review/DecisionReview.cpp
    src/core/LegalNotice.cpp
    src/core/Indicators.cpp
    src/core/ChipDistribution.cpp
    src/core/TradePlan.cpp
    src/core/Auction.cpp
    src/core/RiskFlags.cpp
)
target_include_directories(daban_core_checks PRIVATE src)
target_link_libraries(daban_core_checks PRIVATE Qt6::Core)
if(WIN32)
    target_link_libraries(daban_core_checks PRIVATE crypt32)
endif()
set_target_properties(daban_core_checks PROPERTIES
    WIN32_EXECUTABLE FALSE
    OUTPUT_NAME "DabanQuantCoreChecks"
)

enable_testing()
add_test(NAME core_checks COMMAND daban_core_checks)
set_tests_properties(core_checks PROPERTIES TIMEOUT 30)
add_test(NAME integration_selftest COMMAND daban_selftest --selftest)
set_tests_properties(integration_selftest PROPERTIES TIMEOUT 240 LABELS "integration;network")

# Qt6::Sql 驱动以插件形式随 Qt 安装，发布时需把 sqldrivers 插件一起拷贝到程序目录
