// 纯离线单元门禁（console 入口）：不联网、不依赖 GUI、不依赖数据库，
// 可在任意 CI 上稳定运行。
//
// ★ 断言集本体在 tests/CoreChecks.cpp（CoreChecks::run()）。
//   这里只是薄薄一层入口，因为同一份断言还要被 GUI 目标复用：
//     ① DabanQuantCoreChecks.exe    （本文件，console 目标）
//     ② DabanQuant.exe --corechecks （受管控终端：应用控制策略会拦截新编译的
//       console 可执行文件，GUI 目标在白名单内；没有 ②，门禁在这类机器上形同虚设）

#include <QCoreApplication>
#include "tests/CoreChecks.h"

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    Q_UNUSED(app);
    return CoreChecks::run() == 0 ? 0 : 1;
}
