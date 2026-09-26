#include "icarus/agents/character.h"

namespace icarus {

const char* trait_name_zh(int i) {
    static const char* n[] = {"谨慎", "利他", "野心", "勤勉", "社交", "服从", "攻击性", "理想主义"};
    return (i >= 0 && i < Personality::kCount) ? n[i] : "?";
}

const char* skill_name_zh(int s) {
    static const char* n[] = {"农耕", "建造", "采掘", "搬运", "制作", "研究", "战斗", "医疗", "烹饪"};
    return (s >= 0 && s < kSkillCount) ? n[s] : "?";
}

const char* memory_kind_zh(MemoryKind k) {
    switch (k) {
        case MemoryKind::AteWell: return "吃了顿好饭";
        case MemoryKind::Hungry: return "挨饿";
        case MemoryKind::Thirsty: return "口渴难耐";
        case MemoryKind::Injured: return "受伤";
        case MemoryKind::SawDeath: return "目睹死亡";
        case MemoryKind::FriendDied: return "失去朋友";
        case MemoryKind::Punished: return "受到惩罚";
        case MemoryKind::Rewarded: return "得到奖赏";
        case MemoryKind::Helped: return "得到帮助";
        case MemoryKind::Saved: return "被拯救";
        case MemoryKind::Disaster: return "经历灾难";
        case MemoryKind::HomeLost: return "失去家园";
        case MemoryKind::Rationed: return "被迫配给";
        case MemoryKind::Feast: return "参加盛宴";
        case MemoryKind::Protested: return "参与抗议";
        case MemoryKind::Insulted: return "受辱";
        case MemoryKind::LostJob: return "失去工作";
        case MemoryKind::Healed: return "被治愈";
        case MemoryKind::Blessed: return "受到神恩";
        case MemoryKind::Cursed: return "遭受天罚";
        default: return "";
    }
}

const char* task_name_zh(TaskType t) {
    switch (t) {
        case TaskType::Idle: return "闲着";
        case TaskType::Wander: return "闲逛";
        case TaskType::Eat: return "吃饭";
        case TaskType::Drink: return "喝水";
        case TaskType::Sleep: return "睡觉";
        case TaskType::Socialize: return "交谈";
        case TaskType::Work: return "工作";
        case TaskType::Flee: return "逃离危险";
        case TaskType::Protest: return "抗议";
        case TaskType::Steal: return "偷取食物";
        case TaskType::Heal: return "疗伤";
        case TaskType::Govern: return "处理政务";
        case TaskType::Cast: return "施放魔法";
        case TaskType::Fight: return "战斗";
        case TaskType::Leave: return "出走";
        default: return "无";
    }
}

}  // namespace icarus
