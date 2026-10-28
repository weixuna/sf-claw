/**
 * @file hot_ui.h
 * @brief QuickJS 热重载 UI 的对外接口。
 */

#ifndef HOT_UI_H
#define HOT_UI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOT_UI_SCRIPT_MAX 16384
#define HOT_UI_ERROR_MAX  192

/**
 * @brief 初始化 HotUI 模块。
 *
 * 必须在 GUI 线程、LVGL 初始化完成之后调用。
 *
 * @return 0 成功，负数失败。
 */
int hot_ui_init(void);

/**
 * @brief 提交一段 JavaScript 并等待执行结果。
 *
 * 脚本会在 GUI 线程中编译、运行；如果当前没有界面脚本，会自动打开
 * HotUI App。返回 1 表示脚本已经排队，但等待结果超时。
 *
 * @param name       应用短名，只允许字母、数字、下划线和连字符。
 * @param title      显示标题，可为空。
 * @param code       JavaScript 源码。
 * @param len        源码长度。
 * @param timeout_ms 等待 GUI 执行结果的超时时间，0 表示使用默认值。
 * @param error      错误信息缓冲区，可为空。
 * @param error_size 错误信息缓冲区大小。
 *
 * @return 0 成功，1 已排队，负数失败。
 */
int hot_ui_submit(const char *name, const char *title,
                  const char *code, size_t len,
                  int timeout_ms, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif /* HOT_UI_H */
