/*
 * mod-selfbot-fishing — selfbot fishing commands.
 *
 * This controller reuses mod-playerbots' stock fishing strategy and limits the
 * commands to the player's own active selfbot.
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Item.h"
#include "MotionMaster.h"
#include "Player.h"

#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "FishingAction.h"

#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    struct FishingSession
    {
        bool enabledSelfBot = false;
        bool restoreMainHand = false;
        ObjectGuid previousMainHand;
        ObjectGuid fishingPole;
        std::vector<std::string> suspendedStrategies;
    };

    std::unordered_map<ObjectGuid, FishingSession> fishingSessions;

    Player* GetCommandPlayer(ChatHandler* handler)
    {
        return handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
    }

    void SendPlayerbotMessages(ChatHandler* handler, std::vector<std::string> const& messages)
    {
        for (std::string const& message : messages)
            handler->PSendSysMessage("{}", message.c_str());
    }

    bool ToggleSelfBot(ChatHandler* handler, Player* player)
    {
        if (!sPlayerbotAIConfig.Enabled)
        {
            handler->SendSysMessage("Playerbots are disabled on this server.");
            return false;
        }

        PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(player);
        if (!mgr)
        {
            handler->SendSysMessage("Playerbots could not initialize for this character.");
            return false;
        }

        SendPlayerbotMessages(handler, mgr->HandlePlayerbotCommand("self", player));
        return IsSelfBot(player);
    }

    bool IsFishingPole(Item const* item)
    {
        ItemTemplate const* itemTemplate = item ? item->GetTemplate() : nullptr;
        return itemTemplate && itemTemplate->Class == ITEM_CLASS_WEAPON &&
            itemTemplate->SubClass == ITEM_SUBCLASS_WEAPON_FISHING_POLE;
    }

    Item* FindFishingPole(Player* player)
    {
        Item* mainHand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        if (IsFishingPole(mainHand))
            return mainHand;

        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (IsFishingPole(item))
                    return item;

        for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
            if (Bag* playerBag = player->GetBagByPos(bag))
                for (uint32 slot = 0; slot < playerBag->GetBagSize(); ++slot)
                    if (Item* item = playerBag->GetItemByPos(slot))
                        if (IsFishingPole(item))
                            return item;

        return nullptr;
    }

    PlayerbotAI* GetSelfBotAI(Player* player)
    {
        PlayerbotAI* botAI = player ? GET_PLAYERBOT_AI(player) : nullptr;
        return botAI && IsSelfBot(player) ? botAI : nullptr;
    }

    std::vector<std::string> SuspendRoamingStrategies(PlayerbotAI* botAI)
    {
        static std::vector<std::string> const roamingStrategies = {
            "rpg", "new rpg", "move random", "grind", "quest", "gather", "travel", "follow", "pvp"
        };
        std::vector<std::string> suspended;

        for (std::string const& strategy : roamingStrategies)
        {
            if (!botAI->HasStrategy(strategy, BOT_STATE_NON_COMBAT))
                continue;

            botAI->ChangeStrategy("-" + strategy, BOT_STATE_NON_COMBAT);
            suspended.push_back(strategy);
        }

        return suspended;
    }

    void RestoreStrategies(PlayerbotAI* botAI, std::vector<std::string> const& strategies)
    {
        for (std::string const& strategy : strategies)
            if (!botAI->HasStrategy(strategy, BOT_STATE_NON_COMBAT))
                botAI->ChangeStrategy("+" + strategy, BOT_STATE_NON_COMBAT);
    }

    bool HandleStart(ChatHandler* handler)
    {
        Player* player = GetCommandPlayer(handler);
        if (!player)
            return false;

        if (player->GetSkillValue(SKILL_FISHING) == 0)
        {
            handler->SendSysMessage("Your character needs the Fishing skill before selfbot fishing can start.");
            return true;
        }

        Item* pole = FindFishingPole(player);
        if (!pole)
        {
            handler->SendSysMessage("Put a Fishing Pole in your bags first. Fishing was not started.");
            return true;
        }

        bool enabledHere = false;
        if (!IsSelfBot(player))
        {
            if (GET_PLAYERBOT_AI(player))
            {
                handler->SendSysMessage("This character already has a different Playerbot AI attached.");
                return true;
            }

            if (!ToggleSelfBot(handler, player))
                return true;

            enabledHere = true;
        }

        PlayerbotAI* botAI = GetSelfBotAI(player);
        if (!botAI)
        {
            if (enabledHere)
                ToggleSelfBot(handler, player);
            handler->SendSysMessage("Selfbot activation did not complete; fishing was not started.");
            return true;
        }

        ObjectGuid playerGuid = player->GetGUID();
        auto existingSession = fishingSessions.find(playerGuid);
        FishingSession session = existingSession != fishingSessions.end() ? existingSession->second : FishingSession{};
        if (existingSession == fishingSessions.end())
            session.enabledSelfBot = enabledHere;
        session.fishingPole = pole->GetGUID();

        if (!IsFishingPole(player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND)))
        {
            if (!session.restoreMainHand)
            {
                if (Item* mainHand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
                {
                    session.previousMainHand = mainHand->GetGUID();
                    session.restoreMainHand = true;
                }
            }

            EquipFishingPoleAction equipPole(botAI);
            if (!equipPole.isUseful() || !equipPole.Execute(Event("sbfish start")))
            {
                if (enabledHere)
                    ToggleSelfBot(handler, player);
                handler->SendSysMessage("Could not equip the Fishing Pole; fishing was not started.");
                return true;
            }
        }

        if (existingSession == fishingSessions.end())
            session.suspendedStrategies = SuspendRoamingStrategies(botAI);

        // Selfbot activation can leave a roaming path active. Clear it before
        // the fishing strategy takes control so its first cast is not rejected
        // while the player is still moving.
        player->StopMoving();
        player->GetMotionMaster()->Clear();
        botAI->ChangeStrategy("+master fishing", BOT_STATE_NON_COMBAT);
        fishingSessions[playerGuid] = session;

        handler->SendSysMessage("Selfbot fishing started. Your Fishing Pole is equipped in your main hand.");
        return true;
    }

    bool HandleStop(ChatHandler* handler)
    {
        Player* player = GetCommandPlayer(handler);
        if (!player)
            return false;

        PlayerbotAI* botAI = GetSelfBotAI(player);
        bool wasSelfBotActive = botAI != nullptr;
        if (botAI)
        {
            botAI->ChangeStrategy("-master fishing", BOT_STATE_NON_COMBAT);
            botAI->ChangeStrategy("-use bobber", BOT_STATE_NON_COMBAT);
        }

        auto sessionIt = fishingSessions.find(player->GetGUID());
        bool hadFishingSession = sessionIt != fishingSessions.end();
        bool disableSelfBot = hadFishingSession && sessionIt->second.enabledSelfBot;
        std::vector<std::string> suspendedStrategies;
        if (hadFishingSession)
            suspendedStrategies = sessionIt->second.suspendedStrategies;
        bool restoredMainHand = false;

        if (hadFishingSession)
        {
            player->StopMoving();
            player->GetMotionMaster()->Clear();
        }

        Item* currentMainHand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        if (hadFishingSession && sessionIt->second.restoreMainHand && IsFishingPole(currentMainHand) &&
            currentMainHand->GetGUID() == sessionIt->second.fishingPole)
        {
            if (Item* previousMainHand = player->GetItemByGuid(sessionIt->second.previousMainHand))
            {
                if (Player::IsInventoryPos(previousMainHand->GetPos()))
                {
                    uint16 mainHandPosition = EQUIPMENT_SLOT_MAINHAND | (INVENTORY_SLOT_BAG_0 << 8);
                    player->SwapItem(previousMainHand->GetPos(), mainHandPosition);
                    Item* restored = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
                    restoredMainHand = restored && restored->GetGUID() == sessionIt->second.previousMainHand;
                }
            }
        }

        if (hadFishingSession)
            fishingSessions.erase(sessionIt);

        if (!disableSelfBot && botAI)
            RestoreStrategies(botAI, suspendedStrategies);

        if (disableSelfBot && IsSelfBot(player))
        {
            PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(player);
            if (mgr)
                SendPlayerbotMessages(handler, mgr->HandlePlayerbotCommand("self", player));
        }
        bool selfBotIsActive = IsSelfBot(player);

        if (disableSelfBot)
        {
            if (!selfBotIsActive)
                handler->SendSysMessage("Fishing stopped; the selfbot is off.");
            else
                handler->SendSysMessage("Fishing stopped. Your selfbot remains active.");
        }
        else if (wasSelfBotActive)
            handler->SendSysMessage("Fishing stopped. Your previously active selfbot remains on.");
        else
            handler->SendSysMessage("Selfbot fishing is already stopped.");

        if (restoredMainHand)
            handler->SendSysMessage("Your previous main-hand item has been restored.");
        return true;
    }

    bool HandleStatus(ChatHandler* handler)
    {
        Player* player = GetCommandPlayer(handler);
        PlayerbotAI* botAI = GetSelfBotAI(player);

        if (!botAI)
        {
            handler->SendSysMessage("Selfbot is inactive; use .sbfish start to enable it and begin fishing.");
            return true;
        }

        if (botAI->HasStrategy("master fishing", BOT_STATE_NON_COMBAT))
            handler->SendSysMessage("Selfbot fishing is active.");
        else
            handler->SendSysMessage("Selfbot fishing is inactive. Use .sbfish start to begin.");

        return true;
    }
}

class selfbot_fishing_command_script : public CommandScript
{
public:
    selfbot_fishing_command_script() : CommandScript("selfbot_fishing_command_script") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable sbfishTable =
        {
            { "start",  HandleStart,  SEC_PLAYER, Console::No },
            { "stop",   HandleStop,   SEC_PLAYER, Console::No },
            { "status", HandleStatus, SEC_PLAYER, Console::No },
        };
        static ChatCommandTable root = { { "sbfish", sbfishTable } };
        return root;
    }
};

void AddSC_selfbot_fishing_commands()
{
    new selfbot_fishing_command_script();
}
