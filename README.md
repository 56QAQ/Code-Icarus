# Code:Icarus

以 Godot 为表现层、独立 C++ 内核驱动的 3D 体素空岛神明模拟。你是俯瞰空岛的管理员：人造人居民在这里种田、挑水、盖房、搬运；三位**魔法少女**依各自的**源动力**与性格治理国家、提出主张、进谏、分裂或政变。你可以挖掘、创造、降下陨石、点火、放水——所有干预都会被记入编年史，并沿着真实的物质与因果链影响这个文明。

设计总纲见 [`docs/VISION.md`](docs/VISION.md)，架构说明见 [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)。

## 游玩

### 直接下载（推荐）

每次推送后，GitHub Actions 的 **build** 工作流会构建 Windows / Linux / macOS 版本，作为 artifact（`CodeIcarus-windows` 等）提供下载。解压后运行 `CodeIcarus.exe`（Windows）或 `CodeIcarus.x86_64`（Linux）。

### 从源码运行

需要：CMake ≥ 3.20、Ninja、支持 C++20 的编译器、Python 3 + SCons、Godot 4.5.1。

```bash
git submodule update --init --recursive
tools/build.sh                 # 内核、测试、CLI 与 GDExtension
godot --path game              # 启动游戏
```

Linux 容器可先运行 `tools/setup_env.sh` 安装 Godot、SCons 与无头渲染依赖。

## 操作

| 操作 | 说明 |
|---|---|
| 右键拖动 / 中键拖动 / 滚轮 | 旋转 / 平移 / 缩放镜头 |
| WASD 或方向键 / Q、E | 平移 / 旋转镜头 |
| 左键 | 使用当前工具；「观察」工具下点击居民或建筑查看详情 |
| 底部工具栏 | 观察、挖除、创造、陨石、火焰、洪水（上方弹出范围与材料选项） |
| 空格 / 1–4 | 暂停 / 1×、2×、5×、20× 速度 |
| J | 议事录：魔法少女的每一项决策——她知道的局势、程序给出的可行选项、其他魔法少女的建议、她的选择与理由、事后回顾 |
| C | 编年史：按类别筛选历史，并查看任一事件的因果链图 |
| Esc | 关闭浮层 / 回到观察工具 |

点击右上角的事件提示可以直接追溯其因果；居民卡片的「记忆」与魔法少女的「政见」页也都能跳到对应的事件与决策。

## 魔法少女的决策（Jev）

每个战略决策都走同一条管线：**她能知道的信息 → 程序生成的可行选项（附计算好的数字）→ 决策提供者依源动力与性格抉择 → 本地执行器落实 → 事后回顾并记住结果**。

- **本地人格模型**（默认）：离线、确定性，不需要任何账号。
- **Claude 远程决策**：在议事录（J）右上角打开设置，填入 API Key（或设置环境变量 `ANTHROPIC_API_KEY`），选择模型（默认 `claude-opus-5`）、思考强度与每日调用上限。请求使用结构化输出（JSON Schema 只允许当前可行的选项）并启用了服务器端 fallback（`fallbacks: "default"`）：若请求被安全策略拒绝，会由推荐的备用模型作答。网络错误、超时、无效或过期的答复都会由本地人格模型接管，游戏不会停滞。等待答复时可选择让时间短暂停驻。
- **回放**：决策日志可导出（议事录设置页），载入后按原时刻复现同样的选择，整段历史逐 tick 一致。

## 开发工具

```bash
build/tests/icarus_tests                         # 内核单元/集成测试（含存读档确定性、回放、因果链）
tools/test.sh                                    # 以上 + Godot 冒烟测试 + Jev 客户端对模拟 API 的端到端测试
build/tools/cli/icarus_cli run --seed 3 --days 5 --events --admin 30:break_bridge
build/tools/cli/icarus_cli experiment --seeds 1-8 --days 7 --admin 30:kill_spring --report out.md
tools/screenshot.sh out.png --ticks 1500 --council   # 无头渲染一帧游戏画面
```

`experiment` 在多个种子上施加同样的冲击，报告每个文明的应对、魔法少女的主张与结局（恢复 / 衰落 / 分裂 / 政变）。示例报告见 [`docs/experiments/`](docs/experiments/)。

## 项目状态

第一阶段（一个政权、三位魔法少女、约二十名居民）已可运行：地格生成与休眠、物质物理（水、火、沙、坍塌、陨石）、居民需求与工作、守恒账本与物流、建造与修复、农业与灌溉、魔法少女的治理与政治（进谏、消极抵抗、分裂、政变）、魔法、编年史与因果链、决策记录与回放。科技树、装备与战争在后续阶段推进。
