## 这个 PR 做了什么

<!-- 一两句话。为什么要改比改了什么更重要。 -->

## 检查过了

- [ ] `cmake --preset conda-linux -DENABLE_WERROR=ON` 构建零警告
- [ ] `ctest --preset conda-linux` 全绿
- [ ] e2e 全绿（`tests/e2e/`）
- [ ] 改了界面的话，跑过 `tests/e2e/test_gui_stream.py`

## 如果加了 quirk

- [ ] `src/core/Quirks.cpp` 的表里有条目（key / 分组 / 出处编号 / 参数 / 描述）
- [ ] 有一条 e2e 断言证明这个开关真的改变了行为
- [ ] `scripts/gen-docs.sh` 重新生成过 `docs/quirks.md` 并一起提交
- [ ] 出处编号指向一次真实观测（不凭空造 quirk）

## 如果改了对外行为

- [ ] README 中英两份都更新了
