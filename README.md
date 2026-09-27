# Code:Icarus

以 Godot 为表现层、独立 C++ 内核驱动的 3D 体素空岛神明模拟。你是俯瞰空岛的管理员：人造人居民在这里种田、挑水、盖房、搬运；三位**魔法少女**依各自的**源动力**与性格治理国家、提出主张、进谏、分裂或政变。你可以挖掘、创造、降下陨石、点火、放水，也可以降下粮食、鼓舞或恐吓众人、治愈伤者、降下天雷、唤来降雨、向魔法少女低语——所有干预都会被记入编年史，并沿着真实的物质与因果链影响这个文明。

第二版本把舞台扩展到一座远更大、群系各异的空岛：可以从真正的**蛮荒时代**开局（露宿、采集、狩猎、叶衣兽皮，靠摸索掌握用火、石器、编织、农耕……），也可以让**三个文明**彼此敌对地开局——它们多半先发展自身、开拓新村，在时机合适时宣战、行军、围城，魔法少女随军出征、施展看得见的魔法、彼此对决，最终议和、称臣或结盟。居民会结为伴侣、生儿育女、老去；魔法少女之间结下友谊、竞争、师徒与宿敌的羁绊，经历会让她们的源动力坠落或升华。

设计总纲见 [`docs/VISION.md`](docs/VISION.md)，架构说明见 [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)，第二版本计划与验收结果见 [`docs/V2_PLAN.md`](docs/V2_PLAN.md)。

## 游玩

### 直接下载（推荐）

每次推送后，GitHub Actions 的 **build** 工作流会构建 Windows / Linux / macOS 版本，作为 artifact（`CodeIcarus-windows`、`CodeIcarus-linux`、`CodeIcarus-macos`）提供下载。解压后运行 `CodeIcarus.exe`（Windows）、`CodeIcarus.x86_64`（Linux），macOS 版未签名，首次打开需在「隐私与安全性」中允许。

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
| 底部工具栏 | 观察、挖除、创造、陨石、火焰、洪水、神迹（上方弹出范围、材料或神迹种类） |
| 神迹 | 赐粮（真实粮食记入账本，居民会搬运入库）、鼓舞、恐吓（众人逃离）、治愈（断肢复原，不能起死回生）、天雷、降雨（补满水面、浇熄露天的火） |
| 空格 / 1–4 | 暂停 / 1×、2×、5×、20× 速度 |
| 新空岛（世界菜单） | 地图：经典小岛 / 大岛；开局时代：蛮荒 / 部落 / 村落；文明数 1–3 |
| B | 羁绊：所有魔法少女及其关系网（挚友、对手、师徒、宿敌），圆的大小是追随她的民众多少；悬停查看她的经历，双击打开人物卡 |
| J | 议事录：魔法少女的每一项决策——她知道的局势、程序给出的可行选项、其他魔法少女的建议、她的选择与理由、事后回顾 |
| C | 编年史：按类别筛选历史，并查看任一事件的因果链图 |
| T | 科技树：四个时代的科技、前置关系、当前研究与解锁内容（新时代的科技要先掌握上一时代至少一半的科技）；可「降下启示」 |
| Esc / 顶栏齿轮 | 关闭浮层 / 回到观察工具；无可关闭时打开「世界」菜单：用种子开辟新的空岛、保存到存档槽、读取存档（每个游戏日自动存档一次） |

魔法少女的人物卡有「关系」（她的羁绊）、「魔法」（源动力的来历、心之重负与慰藉）与「传记」（一生的大事，可逐条追溯）。议事之后的几个小时里，魔法少女头顶的名牌会显示她们各自的主张与统治者的裁决。

点击右上角的事件提示可以直接追溯其因果；居民卡片的「记忆」与魔法少女的「政见」页也都能跳到对应的事件与决策。魔法少女「政见」页底部的「神之手」可以向她低语（两天内更在意或不再在意某种价值），或赐予她力量；被低语推动的决策会把这次低语列为起因。

左上角的文明卡片显示时代、研究进度、士兵、战争与通商（点击可追溯宣战、通商或商路受阻的因果）；展开「详情」可看到文明指标（人口、生活水平、知识、生态、稳定）。当一个文明统一全岛，本轮结束：结局卡片会同时列出统一的代价——统一不等于幸福，世界也会继续运转。

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
build/tools/cli/icarus_cli run --layout continent --scenario three_realms --seed 5 --days 32 --every 96 --events
build/tools/cli/icarus_cli run --layout continent --scenario wild --seed 2 --days 30 --events -v   # 蛮荒开局
build/tools/cli/icarus_cli run --load out/r.sav --days 2 -v      # 从存档继续，报告卡顿的 tick
build/tools/cli/icarus_cli experiment --seeds 1-8 --days 7 --admin 30:kill_spring --report out.md
tools/screenshot.sh out.png --ticks 1500 --council   # 无头渲染一帧游戏画面
tools/screenshot.sh out.png --layout continent --scenario three_realms --web   # 羁绊图（--spell-demo heal,rally 可演示法术）
godot --path game --script res://tests/render_test.gd -- --seed 1 --hide-ui  # 本机 GPU 多视角黑面回归（需要显示环境）
```

本机 Vulkan 出现黑面问题的定位、修复与渲染验证见 [`docs/rendering-black-surfaces.md`](docs/rendering-black-surfaces.md)。

`experiment` 在多个种子上施加同样的冲击，报告每个文明的应对、魔法少女的主张与结局（恢复 / 衰落 / 分裂 / 政变 / 战争 / 重归统一），以及林木保有率。示例报告见 [`docs/experiments/`](docs/experiments/)。

## 项目状态

第二版本（大岛、蛮荒开局、三国争霸、生命与世代、野生动物、装备、魔法少女的戏剧、可见的魔法、生活感与雨水表现）已完成，验收实验见 [`docs/V2_PLAN.md`](docs/V2_PLAN.md)。

第一阶段（一个政权、三位魔法少女、约二十名居民）已可运行：地格生成与休眠、物质物理（水、火、沙、坍塌、陨石）、居民需求与工作、守恒账本与物流、建造与修复、农业与灌溉、魔法少女的治理与政治（进谏、消极抵抗、分裂、政变）、魔法、编年史与因果链、决策记录与回放；科技（蛮荒→铁器，含储存、灌溉、运输、生产、医疗与装备）、制作与装备、真实士兵的战争与外交、商队往来的通商与援助、居民迁徙、作用于人心的神迹、统一与文明结局。
