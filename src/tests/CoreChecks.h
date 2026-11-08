#pragma once                                 // 防止头文件被重复包含

// 离线门禁断言集（纯计算、不联网、不依赖 GUI / 数据库）
//
// 断言集被两处复用：
//   1) DabanQuantCoreChecks.exe   —— console 目标，常规 CI 用
//   2) DabanQuant.exe --corechecks —— GUI 目标，用于受管控终端
//      （应用控制策略会拦截新编译的 console 可执行文件，GUI 目标在白名单内）
//
// 两处跑的是同一份断言，不存在两边结果不一样的可能。

namespace CoreChecks {

// 跑完全部离线断言，返回失败断言数（0 = 全部通过）。断言明细写到 stdout。
int run();

} // namespace CoreChecks
