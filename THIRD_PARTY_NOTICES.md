# 第三方组件与许可

本仓库**自身的代码**（`src/*.cpp`、`src/*.rc`、`src/*.h`、`tools/*.py`、`build.sh` 等）采用 **MIT License**，见 [`LICENSE`](LICENSE)。

但构建产物 `texel.exe` 会**内嵌一份字体数据**（由 GNU Unifont 生成），该字体**不是 MIT**，说明如下。

## GNU Unifont（字体）

- 来源：<https://unifoundry.com/unifont/>
- Unifont 的**编译字体是双许可**：
  1. **SIL Open Font License 1.1**（OFL-1.1），以及
  2. **GNU GPLv2+ 附 GNU 字体嵌入例外**。
- 本项目**选用 OFL 1.1**。OFL 明确允许将字体**嵌入任意软件**，且**不要求宿主软件改用 OFL**——因此不影响本项目代码的 MIT 授权。
  OFL 的限制主要针对：单独分发/售卖字体本身，以及"**派生字体**须继续以 OFL 授权"。
- **义务**：凡分发包含该字体的产物（如 `texel.exe`），须**一并附带** Unifont 的版权声明与 OFL 1.1 许可文本。
  本文件与 [`licenses/UNIFONT-LICENSE.txt`](licenses/UNIFONT-LICENSE.txt)（Unifont 官方许可全文：其声明 + GPLv2 全文 + OFL 1.1 全文）即为此用。
- **生成物** `src/font.bin`（由 `unifont.hex` / `unifont_upper.hex` 转换而来）是对字体的**派生字体文件**，许可继承为 **OFL 1.1**；它**不随本仓库分发**（见 `.gitignore`），构建时由使用者自行获取并生成。

## 其它

- 仅使用 Windows 系统库（GDI / GDI+ / IMM / Comdlg32 等），不涉及其它第三方代码。
