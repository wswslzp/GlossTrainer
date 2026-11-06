// Consciousness dust is a player resource, absent from the five item tables.
// Included after the validated localization and player-session helpers.
        void ItemPublish(bool ready, std::string status);
        constexpr std::string_view DustItemKey = "resource:consciousness_dust";
        std::optional<ItemEntry> dustItem;
        std::string dustSession;
        Address dustNameAsset{};

        bool DustFunctionValid(Address function) {
            if (!ObjectValid(function) || ObjectName(function) != "AddDustNum" ||
                ObjectName(Read<Address>(function + 0x20)) != "PBZBackPackSubsystem" ||
                !Executable(Read<Address>(function + 0x128)) || !Executable(base + 0x6619450) ||
                Read<std::uint32_t>(function + 0x100) != 0x04020401 ||
                Read<std::uint8_t>(function + 0x104) != 1 || Read<std::uint16_t>(function + 0x106) != 4) return false;
            const auto parameter = Read<Address>(function + 0x50);
            return Name(Read<std::uint32_t>(parameter + 0x20)) == "Num" &&
                PropertyLayout(parameter, "IntProperty", 4, 0) &&
                Read<std::uint64_t>(parameter + 0x38) == 0x18001040000280ull && !Read<Address>(parameter + 0x10);
        }
        std::optional<std::int32_t> DustAmount(const Context& context) {
            if (!ObjectValid(context.pawn) ||
                !PropertyLayout(FindProperty(Read<Address>(context.pawn + 0x10), "ConsciousnessDust"),
                    "IntProperty", 4, 0x1040)) return {};
            std::int32_t value{};
            if (!ReadBytes(context.pawn + 0x1040, &value, sizeof(value)) || value < 0) return {};
            return value;
        }
        bool RefreshDust(const Context& context) {
            const auto previous = dustItem;
            const auto backpack = itemCatalog.backpack;
            if (!ItemBackpackMatches(context, backpack)) { dustItem.reset(); return previous.has_value(); }
            const auto type = Read<Address>(backpack + 0x10);
            const auto assetProperty = FindProperty(type, "InventoryDataAsset");
            const auto asset = Read<Address>(backpack + 0xbd8);
            const auto session = context.Key();
            if (dustSession != session || dustNameAsset != asset) {
                dustItem.reset(); dustSession = session; dustNameAsset = asset;
            }
            if (!PropertyLayout(assetProperty, "ObjectProperty", 8, 0xbd8) || !ObjectValid(asset) ||
                !ClassNamed(Read<Address>(asset + 0x10), "InventoryData") ||
                !ItemTextField(Read<Address>(asset + 0x10), "DustText", 0x38) || !ItemTextEntriesValid()) {
                dustItem.reset(); return previous.has_value();
            }
            if (!dustItem) {
                std::string key;
                if (!ItemFlowInput(asset + 0x38, key) || key != "UI_SkillWidget_13") return previous.has_value();
                const auto name = ItemResolveText(asset + 0x38);
                if (!name || name->empty() || *name == key || faulted.load() || !ItemBackpackMatches(context, backpack))
                    return previous.has_value();
                ItemEntry item;
                item.key = DustItemKey;
                item.name = ItemFormattedDescription(*name);
                item.category = "资源";
                item.description = "角色持有的资源。输入需要增加的数量并确认，完成后显示实际增加结果。";
                item.maxQuantity = MaxItemQuantity;
                dustItem = std::move(item);
            }
            const auto amount = DustAmount(context);
            const bool callable = DustFunctionValid(FindFunction(type, "AddDustNum"));
            dustItem->owned = amount ? *amount : -1;
            const bool room = amount && *amount < std::numeric_limits<std::int32_t>::max();
            dustItem->available = callable && room;
            dustItem->reason = !amount ? "角色资源数量不可读" : !callable ? "资源增加接口契约不匹配" :
                !room ? "当前数量已达到整数上限" : "";
            return !previous || previous->name != dustItem->name || previous->owned != dustItem->owned ||
                previous->available != dustItem->available || previous->reason != dustItem->reason;
        }
        bool AddDustCapability(const Context& context, std::string& reason) {
            const auto amount = DustAmount(context);
            if (!amount) { reason = "角色资源数量不可读"; return false; }
            const auto backpack = Backpack(context);
            if (backpack && !DustFunctionValid(FindFunction(Read<Address>(backpack + 0x10), "AddDustNum"))) {
                reason = "资源增加接口契约不匹配"; return false;
            }
            if (*amount >= std::numeric_limits<std::int32_t>::max()) {
                reason = "当前数量已达到整数上限"; return false;
            }
            reason = "角色意识尘埃属性或背包增加接口已核对";
            return true;
        }
        ExecutionResult AddDustChecked(const Context& context, int quantity, std::string_view name = "意识尘埃") {
            if (quantity < 1 || quantity > MaxItemQuantity)
                return {RequestState::Failed, "发放数量必须为有效的正整数"};
            const auto backpack = Backpack(context);
            if (!backpack) return {RequestState::Failed, "等待当前玩家背包子系统"};
            const auto backpackIndex = Read<std::uint32_t>(backpack + 0xc);
            const auto before = DustAmount(context);
            if (!before) return {RequestState::Failed, "角色资源数量不可读"};
            if (!ItemQuantityFitsInventory(*before, quantity))
                return {RequestState::Failed, "当前数量加请求数量会溢出，未执行"};
            const auto function = FindFunction(Read<Address>(backpack + 0x10), "AddDustNum");
            if (!DustFunctionValid(function)) return {RequestState::Failed, "资源增加接口契约不匹配"};
            const auto current = CurrentContext();
            if (stopping.load() || faulted.load() || !LocalGameThread() || !current ||
                !current->SameKey(context) || Backpack(*current) != backpack ||
                Read<std::uint32_t>(backpack + 0xc) != backpackIndex)
                return {RequestState::Failed, "资源数量、整数上限或函数契约不符，未执行"};
            auto parameter = static_cast<std::int32_t>(quantity);
            if (!CallProcessEvent(backpack, function, &parameter)) {
                faulted.store(true);
                GetBackend().SetRuntimeState(false, "资源增加发生原生异常，适配器已停用");
                return {RequestState::Indeterminate, "资源增加调用异常，结果无法确认；不会重试"};
            }
            ++dispatchedCalls;
            const auto afterContext = CurrentContext();
            if (!afterContext || !afterContext->SameKey(context) ||
                Backpack(*afterContext) != backpack || Read<std::uint32_t>(backpack + 0xc) != backpackIndex)
                return {RequestState::Indeterminate, "资源增加后玩家会话变化，结果无法确认"};
            const auto after = DustAmount(*afterContext);
            if (itemCatalog.complete && ItemBackpackMatches(*afterContext, itemCatalog.backpack))
                RefreshDust(*afterContext);
            if (after && static_cast<std::int64_t>(*after) - *before == quantity)
                return {RequestState::Verified, std::format("{}：{} → {}，实际增加 {}", name, *before, *after, quantity)};
            return {RequestState::Dispatched, std::format("资源接口已返回，数量 {} → {}；尚未确认精确增量，不自动重试",
                *before, after ? std::to_string(*after) : "未读到")};
        }
        ExecutionResult GrantDust(const Context& context, int quantity) {
            const auto backpack = itemCatalog.backpack;
            if (!itemCatalog.complete || !ItemBackpackMatches(context, backpack) || !dustItem ||
                !dustItem->available || quantity < 1 || quantity > dustItem->maxQuantity)
                return {RequestState::Failed, "当前会话的资源尚未就绪，未执行"};
            return AddDustChecked(context, quantity, dustItem->name);
        }
        ExecutionResult ExecuteAddDust(const Context& context, const Request& request) {
            if (request.arguments.size() != 2 || request.arguments[0] != "add_dust")
                return {RequestState::Failed, "发放数量必须为有效的正整数"};
            int quantity{};
            if (!ParseItemQuantity(request.arguments[1], quantity))
                return {RequestState::Failed, "发放数量必须为有效的正整数"};
            const std::string name = dustItem && !dustItem->name.empty() ? dustItem->name : std::string("意识尘埃");
            auto result = AddDustChecked(context, quantity, name);
            if (!faulted.load() && itemCatalog.complete && ItemBackpackMatches(context, itemCatalog.backpack))
                ItemPublish(true, "物品及角色资源数量已回读");
            return result;
        }
