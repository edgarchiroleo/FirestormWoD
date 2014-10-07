/*
* Copyright (C) 2008-2012 TrinityCore <http://www.trinitycore.org/>
* Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
*
* This program is free software; you can redistribute it and/or modify it
* under the terms of the GNU General Public License as published by the
* Free Software Foundation; either version 2 of the License, or (at your
* option) any later version.
*
* This program is distributed in the hope that it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
* more details.
*
* You should have received a copy of the GNU General Public License along
* with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "Common.h"
#include "Language.h"
#include "DatabaseEnv.h"
#include "WorldPacket.h"
#include "Opcodes.h"
#include "Log.h"
#include "CUFProfiles.h"
#include "Player.h"
#include "GossipDef.h"
#include "World.h"
#include "ObjectMgr.h"
#include "GuildMgr.h"
#include "WorldSession.h"
#include "BigNumber.h"
#include "SHA1.h"
#include "UpdateData.h"
#include "LootMgr.h"
#include "Chat.h"
#include "zlib.h"
#include "ObjectAccessor.h"
#include "Object.h"
#include "Battleground.h"
#include "OutdoorPvP.h"
#include "Pet.h"
#include "SocialMgr.h"
#include "CellImpl.h"
#include "AccountMgr.h"
#include "Vehicle.h"
#include "CreatureAI.h"
#include "DBCEnums.h"
#include "ScriptMgr.h"
#include "MapManager.h"
#include "InstanceScript.h"
#include "GameObjectAI.h"
#include "Group.h"
#include "AccountMgr.h"
#include "Spell.h"
#include "BattlegroundMgr.h"
#include "Battlefield.h"
#include "BattlefieldMgr.h"
#include "TicketMgr.h"

void WorldSession::HandleRepopRequestOpcode(WorldPacket& recvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Recvd CMSG_REPOP_REQUEST Message");

    bool l_CheckInstance = recvData.ReadBit();

    if (GetPlayer()->isAlive() || GetPlayer()->HasFlag(PLAYER_FIELD_PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        return;

    if (GetPlayer()->HasAuraType(SPELL_AURA_PREVENT_RESURRECTION))
        return; // silently return, client should display the error by itself

    // the world update order is sessions, players, creatures
    // the netcode runs in parallel with all of these
    // creatures can kill players
    // so if the server is lagging enough the player can
    // release spirit after he's killed but before he is updated
    if (GetPlayer()->getDeathState() == JUST_DIED)
    {
        sLog->outDebug(LOG_FILTER_NETWORKIO, "HandleRepopRequestOpcode: got request after player %s(%d) was killed and before he was updated", GetPlayer()->GetName(), GetPlayer()->GetGUIDLow());
        GetPlayer()->KillPlayer();
    }

    //this is spirit release confirm?
    GetPlayer()->RemovePet(NULL, PET_SLOT_OTHER_PET, true, true);
    GetPlayer()->BuildPlayerRepop();
    GetPlayer()->RepopAtGraveyard();
}

void WorldSession::HandleGossipSelectOptionOpcode(WorldPacket& recvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: CMSG_GOSSIP_SELECT_OPTION");

    uint32 gossipListId;
    uint32 textId;
    uint64 guid;
    uint32 codeLen = 0;
    std::string code = "";

    recvData.readPackGUID(guid);
    recvData >> textId >> gossipListId;
    codeLen = recvData.ReadBits(8);
    code = recvData.ReadString(codeLen);

    Creature* unit = NULL;
    GameObject* go = NULL;
    if (IS_CRE_OR_VEH_GUID(guid))
    {
        unit = GetPlayer()->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_NONE);
        if (!unit)
        {
            sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: HandleGossipSelectOptionOpcode - Unit (GUID: %u) not found or you can't interact with him.", uint32(GUID_LOPART(guid)));
            return;
        }
    }
    else if (IS_GAMEOBJECT_GUID(guid))
    {
        go = m_Player->GetMap()->GetGameObject(guid);
        if (!go)
        {
            sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: HandleGossipSelectOptionOpcode - GameObject (GUID: %u) not found.", uint32(GUID_LOPART(guid)));
            return;
        }
    }
    else
    {
        sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: HandleGossipSelectOptionOpcode - unsupported GUID type for highguid %u. lowpart %u.", uint32(GUID_HIPART(guid)), uint32(GUID_LOPART(guid)));
        return;
    }

    // remove fake death
    if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
        GetPlayer()->RemoveAurasByType(SPELL_AURA_FEIGN_DEATH);

    if ((unit && unit->GetCreatureTemplate()->ScriptID != unit->LastUsedScriptID) || (go && go->GetGOInfo()->ScriptId != go->LastUsedScriptID))
    {
        sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: HandleGossipSelectOptionOpcode - Script reloaded while in use, ignoring and set new scipt id");
        if (unit)
            unit->LastUsedScriptID = unit->GetCreatureTemplate()->ScriptID;
        if (go)
            go->LastUsedScriptID = go->GetGOInfo()->ScriptId;
        m_Player->PlayerTalkClass->SendCloseGossip();
        return;
    }

    uint32 menuId = m_Player->PlayerTalkClass->GetGossipMenu().GetMenuId();

    if (!code.empty())
    {
        if (unit)
        {
            unit->AI()->sGossipSelectCode(m_Player, menuId, gossipListId, code.c_str());
            if (!sScriptMgr->OnGossipSelectCode(m_Player, unit, m_Player->PlayerTalkClass->GetGossipOptionSender(gossipListId), m_Player->PlayerTalkClass->GetGossipOptionAction(gossipListId), code.c_str()))
                m_Player->OnGossipSelect(unit, gossipListId, menuId);
        }
        else
        {
            go->AI()->GossipSelectCode(m_Player, menuId, gossipListId, code.c_str());
            sScriptMgr->OnGossipSelectCode(m_Player, go, m_Player->PlayerTalkClass->GetGossipOptionSender(gossipListId), m_Player->PlayerTalkClass->GetGossipOptionAction(gossipListId), code.c_str());
        }
    }
    else
    {
        if (unit)
        {
            unit->AI()->sGossipSelect(m_Player, menuId, gossipListId);
            if (!sScriptMgr->OnGossipSelect(m_Player, unit, m_Player->PlayerTalkClass->GetGossipOptionSender(gossipListId), m_Player->PlayerTalkClass->GetGossipOptionAction(gossipListId)))
                m_Player->OnGossipSelect(unit, gossipListId, menuId);
        }
        else
        {
            go->AI()->GossipSelect(m_Player, menuId, gossipListId);
            if (!sScriptMgr->OnGossipSelect(m_Player, go, m_Player->PlayerTalkClass->GetGossipOptionSender(gossipListId), m_Player->PlayerTalkClass->GetGossipOptionAction(gossipListId)))
                m_Player->OnGossipSelect(go, gossipListId, menuId);
        }
    }
}

void WorldSession::HandleWhoOpcode(WorldPacket& recvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Recvd CMSG_WHO Message");

    time_t now = time(NULL);
    if (now - timeLastWhoCommand < 5)
        return;
    else timeLastWhoCommand = now;

    uint32 matchcount = 0;

    uint32 level_min, level_max, racemask, classmask, zones_count, str_count;
    uint32 zoneids[10];                                     // 10 is client limit

    bool unk1, unk2, bit725, bit740;
    uint8 playerLen = 0, guildLen = 0;
    uint8 unkLen2, unkLen3;
    std::string player_name, guild_name;

    recvData >> classmask;                                  // class mask
    recvData >> level_max;                                  // minimal player level, default 100 (MAX_LEVEL)
    recvData >> level_min;                                  // maximal player level, default 0
    recvData >> racemask;                                   // race mask

    guildLen = recvData.ReadBits(7);
    playerLen = recvData.ReadBits(6);
    str_count = recvData.ReadBits(3);

    if (str_count > 4)                                      // can't be received from real client or broken packet
        return;

    unk1 = recvData.ReadBit();
    unk2 = recvData.ReadBit();
    zones_count = recvData.ReadBits(4);                     // zones count, client limit = 10 (2.0.10)

    if (zones_count > 10)                                   // can't be received from real client or broken packet
        return;

    unkLen2 = recvData.ReadBits(8) << 1;
    unkLen2 += recvData.ReadBit();
    unkLen3 = recvData.ReadBits(8) << 1;
    unkLen3 += recvData.ReadBit();
    bit725 = recvData.ReadBit();

    uint8* unkLens = new uint8[str_count];
    std::string* unkStrings = new std::string[str_count];

    for (uint8 i = 0; i < str_count; i++)
        unkLens[i] = recvData.ReadBits(7);

    bit740 = recvData.ReadBit();
    recvData.FlushBits();

    if (playerLen > 0)
        player_name = recvData.ReadString(playerLen);       // player name, case sensitive...

    std::wstring str[4];                                    // 4 is client limit
    for (uint32 i = 0; i < str_count; ++i)
    {
        std::string temp = recvData.ReadString(unkLens[i]); // user entered string, it used as universal search pattern(guild+player name)?
        if (!Utf8toWStr(temp, str[i]))
            continue;

        wstrToLower(str[i]);
        sLog->outDebug(LOG_FILTER_NETWORKIO, "String %u: %s", i, temp.c_str());
    }

    if (guildLen > 0)
        guild_name = recvData.ReadString(guildLen); // guild name, case sensitive ...

    for (uint32 i = 0; i < zones_count; ++i)
    {
        uint32 temp;
        recvData >> temp;                                   // zone id, 0 if zone is unknown...
        zoneids[i] = temp;
        sLog->outDebug(LOG_FILTER_NETWORKIO, "Zone %u: %u", i, zoneids[i]);
    }

    if (unkLen3 > 0)
        std::string unkString = recvData.ReadString(unkLen3);

    if (unkLen2 > 0)
        std::string unkString = recvData.ReadString(unkLen2);

    if (bit740)
    {
        uint32 unk1, unk2, unk3;
        recvData >> unk1 >> unk2 >> unk3;
    }

    sLog->outDebug(LOG_FILTER_NETWORKIO, "Minlvl %u, maxlvl %u, name %s, guild %s, racemask %u, classmask %u, zones %u, strings %u", level_min, level_max, player_name.c_str(), guild_name.c_str(), racemask, classmask, zones_count, str_count);

    std::wstring wplayer_name;
    std::wstring wguild_name;
    if (!(Utf8toWStr(player_name, wplayer_name) && Utf8toWStr(guild_name, wguild_name)))
        return;
    wstrToLower(wplayer_name);
    wstrToLower(wguild_name);

    // client send in case not set max level value 100 but Trinity supports 255 max level,
    // update it to show GMs with characters after 100 level
    if (level_max >= MAX_LEVEL)
        level_max = STRONG_MAX_LEVEL;

    uint32 team = m_Player->GetTeam();
    uint32 security = GetSecurity();
    bool allowTwoSideWhoList = sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_WHO_LIST);
    uint32 gmLevelInWhoList  = sWorld->getIntConfig(CONFIG_GM_LEVEL_IN_WHO_LIST);
    uint8 displaycount = 0;

    ByteBuffer bitsData;
    ByteBuffer bytesData;
    WorldPacket data(SMSG_WHO);
    size_t pos = data.wpos();

    bitsData.WriteBits(displaycount, 6);

    TRINITY_READ_GUARD(HashMapHolder<Player>::LockType, *HashMapHolder<Player>::GetLock());
    HashMapHolder<Player>::MapType const& m = sObjectAccessor->GetPlayers();
    for (HashMapHolder<Player>::MapType::const_iterator itr = m.begin(); itr != m.end(); ++itr)
    {
        if (AccountMgr::IsPlayerAccount(security))
        {
            // player can see member of other team only if CONFIG_ALLOW_TWO_SIDE_WHO_LIST
            if (itr->second->GetTeam() != team && !allowTwoSideWhoList)
                continue;

            // player can see MODERATOR, GAME MASTER, ADMINISTRATOR only if CONFIG_GM_IN_WHO_LIST
            if ((itr->second->GetSession()->GetSecurity() > AccountTypes(gmLevelInWhoList)))
                continue;
        }

        //do not process players which are not in world
        if (!(itr->second->IsInWorld()))
            continue;

        // check if target is globally visible for player
        if (!(itr->second->IsVisibleGloballyFor(m_Player)))
            continue;

        // check if target's level is in level range
        uint8 lvl = itr->second->getLevel();
        if (lvl < level_min || lvl > level_max)
            continue;

        // check if class matches classmask
        uint32 class_ = itr->second->getClass();
        if (!(classmask & (1 << class_)))
            continue;

        // check if race matches racemask
        uint32 race = itr->second->getRace();
        if (!(racemask & (1 << race)))
            continue;

        uint32 pzoneid = itr->second->GetZoneId();
        uint8 gender = itr->second->getGender();

        bool z_show = true;
        for (uint32 i = 0; i < zones_count; ++i)
        {
            if (zoneids[i] == pzoneid)
            {
                z_show = true;
                break;
            }

            z_show = false;
        }
        if (!z_show)
            continue;

        std::string pname = itr->second->GetName();
        std::wstring wpname;
        if (!Utf8toWStr(pname, wpname))
            continue;
        wstrToLower(wpname);

        if (!(wplayer_name.empty() || wpname.find(wplayer_name) != std::wstring::npos))
            continue;

        std::string gname = sGuildMgr->GetGuildNameById(itr->second->GetGuildId());
        std::wstring wgname;
        if (!Utf8toWStr(gname, wgname))
            continue;
        wstrToLower(wgname);

        if (!(wguild_name.empty() || wgname.find(wguild_name) != std::wstring::npos))
            continue;

        std::string aname;
        if (AreaTableEntry const* areaEntry = GetAreaEntryByAreaID(itr->second->GetZoneId()))
            aname = areaEntry->area_name[GetSessionDbcLocale()];

        bool s_show = true;
        for (uint32 i = 0; i < str_count; ++i)
        {
            if (!str[i].empty())
            {
                if (wgname.find(str[i]) != std::wstring::npos ||
                    wpname.find(str[i]) != std::wstring::npos ||
                    Utf8FitTo(aname, str[i]))
                {
                    s_show = true;
                    break;
                }
                s_show = false;
            }
        }
        if (!s_show)
            continue;

        // 49 is maximum player count sent to client - can be overridden
        // through config, but is unstable
        if ((matchcount++) >= sWorld->getIntConfig(CONFIG_MAX_WHO))
        {
            if (sWorld->getBoolConfig(CONFIG_LIMIT_WHO_ONLINE))
                break;
            else
                continue;

            break;
        }

        ObjectGuid playerGuid = itr->second->GetGUID();
        ObjectGuid unkGuid = 0;
        ObjectGuid guildGuid = itr->second->GetGuild() ? itr->second->GetGuild()->GetGUID() : 0;

        bitsData.WriteBit(guildGuid[4]);

        if (DeclinedName const* names = itr->second->GetDeclinedNames())
        {
            for (uint8 i = 0; i < MAX_DECLINED_NAME_CASES; ++i)
                bitsData.WriteBits(names->name[i].size(), 7);
        }
        else
        {
            for (uint8 i = 0; i < MAX_DECLINED_NAME_CASES; ++i)
                bitsData.WriteBits(0, 7);
        }

        bitsData.WriteBits(gname.size(), 7);
        bitsData.WriteBit(unkGuid[3]);
        bitsData.WriteBit(playerGuid[1]);
        bitsData.WriteBits(pname.size(), 6);
        bitsData.WriteBit(guildGuid[2]);
        bitsData.WriteBit(playerGuid[7]);
        bitsData.WriteBit(unkGuid[2]);
        bitsData.WriteBit(guildGuid[6]);
        bitsData.WriteBit(guildGuid[7]);
        bitsData.WriteBit(unkGuid[7]);
        bitsData.WriteBit(playerGuid[3]);
        bitsData.WriteBit(unkGuid[4]);
        bitsData.WriteBit(0);
        bitsData.WriteBit(unkGuid[1]);
        bitsData.WriteBit(unkGuid[0]);
        bitsData.WriteBit(playerGuid[4]);
        bitsData.WriteBit(0);
        bitsData.WriteBit(guildGuid[1]);
        bitsData.WriteBit(guildGuid[3]);
        bitsData.WriteBit(unkGuid[6]);
        bitsData.WriteBit(guildGuid[0]);
        bitsData.WriteBit(playerGuid[2]);
        bitsData.WriteBit(playerGuid[5]);
        bitsData.WriteBit(playerGuid[6]);
        bitsData.WriteBit(guildGuid[5]);
        bitsData.WriteBit(unkGuid[5]);
        bitsData.WriteBit(playerGuid[0]);

        bytesData.WriteByteSeq(playerGuid[7]);
        bytesData.WriteByteSeq(guildGuid[4]);
        bytesData.WriteByteSeq(guildGuid[1]);
        bytesData << uint32(38297239);
        bytesData.WriteByteSeq(playerGuid[0]);

        if (pname.size() > 0)
            bytesData.append(pname.c_str(), pname.size());

        bytesData.WriteByteSeq(unkGuid[0]);
        bytesData.WriteByteSeq(guildGuid[6]);
        bytesData << uint32(pzoneid);
        bytesData.WriteByteSeq(unkGuid[7]);
        bytesData.WriteByteSeq(unkGuid[5]);
        bytesData.WriteByteSeq(playerGuid[3]);
        bytesData.WriteByteSeq(playerGuid[6]);
        bytesData << uint32(realmID);
        bytesData << uint8(race);
        bytesData << uint8(lvl);
        bytesData.WriteByteSeq(playerGuid[2]);
        bytesData.WriteByteSeq(playerGuid[1]);
        bytesData << uint8(gender);
        bytesData << uint32(realmID);
        bytesData.WriteByteSeq(guildGuid[0]);
        bytesData.WriteByteSeq(guildGuid[5]);
        bytesData.WriteByteSeq(guildGuid[7]);

        if (DeclinedName const* names = itr->second->GetDeclinedNames())
            for (uint8 i = 0; i < MAX_DECLINED_NAME_CASES; ++i)
                if (names->name[i].size() > 0)
                    bytesData.append(names->name[i].c_str(), names->name[i].size());

        bytesData.WriteByteSeq(playerGuid[5]);
        bytesData.WriteByteSeq(guildGuid[3]);
        bytesData.WriteByteSeq(playerGuid[4]);

        if (gname.size() > 0)
            bytesData.append(gname.c_str(), gname.size());

        bytesData << uint8(class_);
        bytesData.WriteByteSeq(unkGuid[4]);
        bytesData.WriteByteSeq(guildGuid[2]);
        bytesData.WriteByteSeq(unkGuid[3]);
        bytesData.WriteByteSeq(unkGuid[6]);
        bytesData.WriteByteSeq(unkGuid[2]);
        bytesData.WriteByteSeq(unkGuid[1]);

        ++displaycount;
    }

    if (displaycount != 0)
    {
        bitsData.FlushBits();
        uint8 firstByte = bitsData.contents()[0];
        data << uint8(displaycount << 2 | firstByte & 0x3);
        for (size_t i = 1; i < bitsData.size(); i++)
            data << uint8(bitsData.contents()[i]);

        data.append(bytesData);
    }
    else
        data.WriteBits(0, 6);

    SendPacket(&data);

    delete[] unkLens;
    delete[] unkStrings;

    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Send SMSG_WHO Message");
}

void WorldSession::HandleLogoutRequestOpcode(WorldPacket& /*recvData*/)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Recvd CMSG_LOGOUT_REQUEST Message, security - %u", GetSecurity());

    if (uint64 lguid = GetPlayer()->GetLootGUID())
        DoLootRelease(lguid);

    uint32 reason = 0;

    if (GetPlayer()->isInCombat())
        reason = 1;
    else if (GetPlayer()->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR))
        reason = 3;                                         // is jumping or falling
    else if (GetPlayer()->duel || GetPlayer()->HasAura(9454)) // is dueling or frozen by GM via freeze command
        reason = 2;                                         // FIXME - Need the correct value

    if (reason)
    {
        WorldPacket data(SMSG_LOGOUT_RESPONSE, 1+4);
        data << uint32(reason);
        data.WriteBit(0);
        data.FlushBits();
        SendPacket(&data);
        LogoutRequest(0);
        return;
    }

    // Instant logout in taverns/cities or on taxi or for admins, gm's, mod's if its enabled in worldserver.conf
    if (GetPlayer()->HasFlag(PLAYER_FIELD_PLAYER_FLAGS, PLAYER_FLAGS_RESTING) || GetPlayer()->isInFlight() ||
        GetSecurity() >= AccountTypes(sWorld->getIntConfig(CONFIG_INSTANT_LOGOUT)))
    {
        WorldPacket data(SMSG_LOGOUT_RESPONSE, 1+4);
        data << uint32(reason);
        data.WriteBit(1);           // instant logout
        data.FlushBits();
        SendPacket(&data);
        LogoutPlayer(true);
        return;
    }

    // not set flags if player can't free move to prevent lost state at logout cancel
    if (GetPlayer()->CanFreeMove())
    {
        GetPlayer()->SetStandState(UNIT_STAND_STATE_SIT);
        GetPlayer()->SetRooted(true);
        GetPlayer()->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED);
    }

    WorldPacket data(SMSG_LOGOUT_RESPONSE, 1+4);
    data << uint32(0);
    data.WriteBit(0);
    data.FlushBits();
    SendPacket(&data);
    LogoutRequest(time(NULL));
}

void WorldSession::HandlePlayerLogoutOpcode(WorldPacket& recvData)
{
    bool unkBit = !recvData.ReadBit();

    recvData.FlushBits();

    uint32 unk = 0;
    if (unkBit)
        unk = recvData.read<uint32>();

    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Recvd CMSG_PLAYER_LOGOUT Message");
}

void WorldSession::HandleLogoutCancelOpcode(WorldPacket& /*recvData*/)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Recvd CMSG_LOGOUT_CANCEL Message");

    // Player have already logged out serverside, too late to cancel
    if (!GetPlayer())
        return;

    LogoutRequest(0);

    WorldPacket data(SMSG_LOGOUT_CANCEL_ACK, 0);
    SendPacket(&data);

    // not remove flags if can't free move - its not set in Logout request code.
    if (GetPlayer()->CanFreeMove())
    {
        //!we can move again
        GetPlayer()->SetRooted(false);

        //! Stand Up
        GetPlayer()->SetStandState(UNIT_STAND_STATE_STAND);

        //! DISABLE_ROTATE
        GetPlayer()->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED);
    }

    GetPlayer()->PetSpellInitialize();

    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Sent SMSG_LOGOUT_CANCEL_ACK Message");
}

void WorldSession::HandleTogglePvP(WorldPacket& recvData)
{
    // this opcode can be used in two ways: Either set explicit new status or toggle old status
    if (recvData.size() == 1)
    {
        bool newPvPStatus = recvData.ReadBit();

        GetPlayer()->ApplyModFlag(PLAYER_FIELD_PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP, newPvPStatus);
        GetPlayer()->ApplyModFlag(PLAYER_FIELD_PLAYER_FLAGS, PLAYER_FLAGS_PVP_TIMER, !newPvPStatus);
    }
    else
    {
        GetPlayer()->ToggleFlag(PLAYER_FIELD_PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP);
        GetPlayer()->ToggleFlag(PLAYER_FIELD_PLAYER_FLAGS, PLAYER_FLAGS_PVP_TIMER);
    }

    if (GetPlayer()->HasFlag(PLAYER_FIELD_PLAYER_FLAGS, PLAYER_FLAGS_IN_PVP))
    {
        if (!GetPlayer()->IsPvP() || GetPlayer()->pvpInfo.endTimer != 0)
            GetPlayer()->UpdatePvP(true, true);
    }
    else
    {
        if (!GetPlayer()->pvpInfo.inHostileArea && GetPlayer()->IsPvP())
            GetPlayer()->pvpInfo.endTimer = time(NULL);     // start toggle-off
    }

    //if (OutdoorPvP* pvp = _player->GetOutdoorPvP())
    //    pvp->HandlePlayerActivityChanged(_player);
}

void WorldSession::HandleZoneUpdateOpcode(WorldPacket& recvData)
{
    uint32 newZone;
    recvData >> newZone;

    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Recvd ZONE_UPDATE: %u", newZone);

    // use server size data
    uint32 newzone, newarea;
    GetPlayer()->GetZoneAndAreaId(newzone, newarea);
    GetPlayer()->UpdateZone(newzone, newarea);
    //GetPlayer()->SendInitWorldStates(true, newZone);
}

void WorldSession::HandleReturnToGraveyard(WorldPacket& /*recvPacket*/)
{
    if (GetPlayer()->isAlive() || !GetPlayer()->HasFlag(PLAYER_FIELD_PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        return;
    //TODO: unk32, unk32
    GetPlayer()->RepopAtGraveyard();
}

void WorldSession::HandleSetSelectionOpcode(WorldPacket& p_RecvData)
{
    uint64 l_NewTargetGuid;

    p_RecvData.readPackGUID(l_NewTargetGuid);

    m_Player->SetSelection(l_NewTargetGuid);
}

void WorldSession::HandleStandStateChangeOpcode(WorldPacket& recvData)
{
    uint32 l_StandState;
    recvData >> l_StandState;

    m_Player->SetStandState(l_StandState);
}

void WorldSession::HandleContactListOpcode(WorldPacket& p_RecvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Received CMSG_SEND_CONTACT_LIST");

    uint32 l_Flags;

    p_RecvData >> l_Flags;

    if (l_Flags & 1)
        m_Player->GetSocial()->SendSocialList(m_Player);
}

void WorldSession::HandleAddFriendOpcode(WorldPacket& p_RecvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Received CMSG_ADD_FRIEND");

    std::string l_FriendName = GetTrinityString(LANG_FRIEND_IGNORE_UNKNOWN);
    std::string l_FriendNote;

    uint32 l_NameLen = p_RecvData.ReadBits(9);
    uint32 l_NoteLen = p_RecvData.ReadBits(10);

    l_FriendName = p_RecvData.ReadString(l_NameLen);
    l_FriendNote = p_RecvData.ReadString(l_NoteLen);

    if (!normalizePlayerName(l_FriendName))
        return;

    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: %s asked to add friend : '%s'", GetPlayer()->GetName(), l_FriendName.c_str());

    PreparedStatement* l_Stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_GUID_RACE_ACC_BY_NAME);

    l_Stmt->setString(0, l_FriendName);

    _addFriendCallback.SetParam(l_FriendNote);
    _addFriendCallback.SetFutureResult(CharacterDatabase.AsyncQuery(l_Stmt));
}

void WorldSession::HandleAddFriendOpcodeCallBack(PreparedQueryResult result, std::string friendNote)
{
    if (!GetPlayer())
        return;

    uint64 friendGuid;
    uint32 friendAccountId;
    uint32 team;
    FriendsResult friendResult;

    friendResult = FRIEND_NOT_FOUND;
    friendGuid = 0;

    if (result)
    {
        Field* fields = result->Fetch();

        friendGuid = MAKE_NEW_GUID(fields[0].GetUInt32(), 0, HIGHGUID_PLAYER);
        team = Player::TeamForRace(fields[1].GetUInt8());
        friendAccountId = fields[2].GetUInt32();

        if (!AccountMgr::IsPlayerAccount(GetSecurity()) || sWorld->getBoolConfig(CONFIG_ALLOW_GM_FRIEND) || AccountMgr::IsPlayerAccount(AccountMgr::GetSecurity(friendAccountId, realmID)))
        {
            if (friendGuid)
            {
                if (friendGuid == GetPlayer()->GetGUID())
                    friendResult = FRIEND_SELF;
                else if (GetPlayer()->GetTeam() != team && !sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_ADD_FRIEND) && AccountMgr::IsPlayerAccount(GetSecurity()))
                    friendResult = FRIEND_ENEMY;
                else if (GetPlayer()->GetSocial()->HasFriend(GUID_LOPART(friendGuid)))
                    friendResult = FRIEND_ALREADY;
                else
                {
                    Player* pFriend = ObjectAccessor::FindPlayer(friendGuid);
                    if (pFriend && pFriend->IsInWorld() && pFriend->IsVisibleGloballyFor(GetPlayer()))
                        friendResult = FRIEND_ADDED_ONLINE;
                    else
                        friendResult = FRIEND_ADDED_OFFLINE;
                    if (!GetPlayer()->GetSocial()->AddToSocialList(GUID_LOPART(friendGuid), false))
                    {
                        friendResult = FRIEND_LIST_FULL;
                        sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: %s's friend list is full.", GetPlayer()->GetName());
                    }
                }
                GetPlayer()->GetSocial()->SetFriendNote(GUID_LOPART(friendGuid), friendNote);
            }
        }
    }

    sSocialMgr->SendFriendStatus(GetPlayer(), friendResult, GUID_LOPART(friendGuid), false);

    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Sent (SMSG_FRIEND_STATUS)");
}

void WorldSession::HandleDelFriendOpcode(WorldPacket& recvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Received CMSG_DEL_FRIEND");

    uint64 l_Guid;

    uint32 l_VirtualRealmAddress;

    recvData >> l_VirtualRealmAddress;
    recvData.readPackGUID(l_Guid);

    m_Player->GetSocial()->RemoveFromSocialList(GUID_LOPART(l_Guid), false);

    sSocialMgr->SendFriendStatus(GetPlayer(), FRIEND_REMOVED, GUID_LOPART(l_Guid), false);
}

void WorldSession::HandleAddIgnoreOpcode(WorldPacket& p_RecvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Received CMSG_ADD_IGNORE");

    std::string l_IgnoreName = GetTrinityString(LANG_FRIEND_IGNORE_UNKNOWN);

    uint32 l_IgnoreNameLen = p_RecvData.ReadBits(9);

    l_IgnoreName = p_RecvData.ReadString(l_IgnoreNameLen);

    if (!normalizePlayerName(l_IgnoreName))
        return;

    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: %s asked to Ignore: '%s'", GetPlayer()->GetName(), l_IgnoreName.c_str());

    PreparedStatement* l_Stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_GUID_BY_NAME);

    l_Stmt->setString(0, l_IgnoreName);

    _addIgnoreCallback = CharacterDatabase.AsyncQuery(l_Stmt);
}

void WorldSession::HandleAddIgnoreOpcodeCallBack(PreparedQueryResult result)
{
    if (!GetPlayer())
        return;

    uint64 IgnoreGuid;
    FriendsResult ignoreResult;

    ignoreResult = FRIEND_IGNORE_NOT_FOUND;
    IgnoreGuid = 0;

    if (result)
    {
        IgnoreGuid = MAKE_NEW_GUID((*result)[0].GetUInt32(), 0, HIGHGUID_PLAYER);

        if (IgnoreGuid)
        {
            if (IgnoreGuid == GetPlayer()->GetGUID())              //not add yourself
                ignoreResult = FRIEND_IGNORE_SELF;
            else if (GetPlayer()->GetSocial()->HasIgnore(GUID_LOPART(IgnoreGuid)))
                ignoreResult = FRIEND_IGNORE_ALREADY;
            else
            {
                ignoreResult = FRIEND_IGNORE_ADDED;

                // ignore list full
                if (!GetPlayer()->GetSocial()->AddToSocialList(GUID_LOPART(IgnoreGuid), true))
                    ignoreResult = FRIEND_IGNORE_FULL;
            }
        }
    }

    sSocialMgr->SendFriendStatus(GetPlayer(), ignoreResult, GUID_LOPART(IgnoreGuid), false);

    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Sent (SMSG_FRIEND_STATUS)");
}

void WorldSession::HandleDelIgnoreOpcode(WorldPacket& recvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Received CMSG_DEL_IGNORE");

    uint64 l_Guid;

    uint32 l_VirtualRealmAddress;

    recvData >> l_VirtualRealmAddress;
    recvData.readPackGUID(l_Guid);

    m_Player->GetSocial()->RemoveFromSocialList(GUID_LOPART(l_Guid), true);

    sSocialMgr->SendFriendStatus(GetPlayer(), FRIEND_IGNORE_REMOVED, GUID_LOPART(l_Guid), false);
}

void WorldSession::HandleSetContactNotesOpcode(WorldPacket& p_RecvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "CMSG_SET_CONTACT_NOTES");

    uint64 l_Guid;
    uint32 l_VirtualRealmAddress;

    std::string l_Notes;

    p_RecvData >> l_VirtualRealmAddress;
    p_RecvData.readPackGUID(l_Guid);
    
    l_Notes = p_RecvData.ReadString(p_RecvData.ReadBits(10));

    m_Player->GetSocial()->SetFriendNote(GUID_LOPART(l_Guid), l_Notes);
}

void WorldSession::HandleReportBugOpcode(WorldPacket& recvData)
{
    float posX, posY, posZ, orientation;
    uint32 contentlen, mapId;
    std::string content;

    recvData >> posX >> posY >> orientation >> posZ;
    recvData >> mapId;

    contentlen = recvData.ReadBits(10);
    recvData.FlushBits();
    content = recvData.ReadString(contentlen);

    sLog->outDebug(LOG_FILTER_NETWORKIO, "%s", content.c_str());

    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_BUG_REPORT);
    stmt->setString(0, "Bug");
    stmt->setString(1, content);
    CharacterDatabase.Execute(stmt);
}

void WorldSession::HandleReportSuggestionOpcode(WorldPacket& recvData)
{
    float posX, posY, posZ, orientation;
    uint32 contentlen, mapId;
    std::string content;

    recvData >> mapId;
    recvData >> posZ >> orientation >> posY >> posX;

    contentlen = recvData.ReadBits(10);
    recvData.FlushBits();
    content = recvData.ReadString(contentlen);

    sLog->outDebug(LOG_FILTER_NETWORKIO, "%s", content.c_str());

    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_BUG_REPORT);
    stmt->setString(0, "Suggestion");
    stmt->setString(1, content);
    CharacterDatabase.Execute(stmt);
}

void WorldSession::HandleRequestBattlePetJournal(WorldPacket& /*recvPacket*/)
{
    WorldPacket data;
    GetPlayer()->GetBattlePetMgr().BuildBattlePetJournal(&data);
    SendPacket(&data);
}

void WorldSession::HandleRequestGmTicket(WorldPacket& /*recvPakcet*/)
{
    // Notify player if he has a ticket in progress
    if (GmTicket* ticket = sTicketMgr->GetTicketByPlayer(GetPlayer()->GetGUID()))
    {
        if (ticket->IsCompleted())
            ticket->SendResponse(this);
        else
            sTicketMgr->SendTicket(this, ticket);
    }
}

void WorldSession::HandleReclaimCorpseOpcode(WorldPacket& p_Packet)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Received CMSG_RECLAIM_CORPSE");

    uint64 l_CorpseGUID = 0;

    p_Packet.readPackGUID(l_CorpseGUID);
    
    if (GetPlayer()->isAlive())
        return;

    /// Do not allow corpse reclaim in arena
    if (GetPlayer()->InArena())
        return;

    /// Body not released yet
    if (!GetPlayer()->HasFlag(PLAYER_FIELD_PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
        return;

    Corpse * l_Corpse = GetPlayer()->GetCorpse();

    if (!l_Corpse)
        return;

    /// Prevent resurrect before 30-sec delay after body release not finished
    if (time_t(l_Corpse->GetGhostTime() + GetPlayer()->GetCorpseReclaimDelay(l_Corpse->GetType() == CORPSE_RESURRECTABLE_PVP)) > time_t(time(NULL)))
        return;

    if (!l_Corpse->IsWithinDistInMap(GetPlayer(), CORPSE_RECLAIM_RADIUS, true))
        return;

    /// Resurrect
    GetPlayer()->ResurrectPlayer(GetPlayer()->InBattleground() ? 1.0f : 0.5f);

    /// Spawn bones
    GetPlayer()->SpawnCorpseBones();
}

void WorldSession::HandleResurrectResponseOpcode(WorldPacket& recvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Received CMSG_RESURRECT_RESPONSE");

    uint32 status;
    recvData >> status;

    ObjectGuid guid;

    uint8 bitsOrder[8] = { 3, 4, 1, 5, 2, 0, 7, 6 };
    recvData.ReadBitInOrder(guid, bitsOrder);

    recvData.FlushBits();

    uint8 bytesOrder[8] = { 0, 6, 4, 5, 3, 1, 2, 7 };
    recvData.ReadBytesSeq(guid, bytesOrder);

    if (GetPlayer()->isAlive())
        return;

    if (status == 1)
    {
        GetPlayer()->ClearResurrectRequestData();           // reject
        return;
    }

    if (!GetPlayer()->IsRessurectRequestedBy(guid))
        return;

    GetPlayer()->ResurectUsingRequestData();
}

void WorldSession::SendAreaTriggerMessage(const char* Text, ...)
{
    va_list ap;
    char szStr [1024];
    szStr[0] = '\0';

    va_start(ap, Text);
    vsnprintf(szStr, 1024, Text, ap);
    va_end(ap);

    uint32 length = strlen(szStr)+1;
    WorldPacket data(SMSG_AREA_TRIGGER_MESSAGE, 4+length);
    data << length;
    data << szStr;
    SendPacket(&data);
}

void WorldSession::HandleAreaTriggerOpcode(WorldPacket& recvData)
{
    uint32 triggerId;
    uint8 unkbit1, unkbit2;

    recvData >> triggerId;
    unkbit1 = recvData.ReadBit();
    unkbit2 = recvData.ReadBit();

    sLog->outDebug(LOG_FILTER_NETWORKIO, "CMSG_AREATRIGGER. Trigger ID: %u", triggerId);

    Player* player = GetPlayer();
    if (player->isInFlight())
    {
        sLog->outDebug(LOG_FILTER_NETWORKIO, "HandleAreaTriggerOpcode: Player '%s' (GUID: %u) in flight, ignore Area Trigger ID:%u",
            player->GetName(), player->GetGUIDLow(), triggerId);
        return;
    }

    AreaTriggerEntry const* atEntry = sAreaTriggerStore.LookupEntry(triggerId);
    if (!atEntry)
    {
        sLog->outDebug(LOG_FILTER_NETWORKIO, "HandleAreaTriggerOpcode: Player '%s' (GUID: %u) send unknown (by DBC) Area Trigger ID:%u",
            player->GetName(), player->GetGUIDLow(), triggerId);
        return;
    }

    if (player->GetMapId() != atEntry->mapid)
    {
        sLog->outDebug(LOG_FILTER_NETWORKIO, "HandleAreaTriggerOpcode: Player '%s' (GUID: %u) too far (trigger map: %u player map: %u), ignore Area Trigger ID: %u",
            player->GetName(), atEntry->mapid, player->GetMapId(), player->GetGUIDLow(), triggerId);
        return;
    }

    // delta is safe radius
    const float delta = 5.0f;

    if (atEntry->radius > 0)
    {
        // if we have radius check it
        float dist = player->GetDistance(atEntry->x, atEntry->y, atEntry->z);
        if (dist > atEntry->radius + delta)
        {
            sLog->outDebug(LOG_FILTER_NETWORKIO, "HandleAreaTriggerOpcode: Player '%s' (GUID: %u) too far (radius: %f distance: %f), ignore Area Trigger ID: %u",
                player->GetName(), player->GetGUIDLow(), atEntry->radius, dist, triggerId);
            return;
        }
    }
    else
    {
        // we have only extent

        // rotate the players position instead of rotating the whole cube, that way we can make a simplified
        // is-in-cube check and we have to calculate only one point instead of 4

        // 2PI = 360°, keep in mind that ingame orientation is counter-clockwise
        double rotation = 2 * M_PI - atEntry->box_orientation;
        double sinVal = std::sin(rotation);
        double cosVal = std::cos(rotation);

        float playerBoxDistX = player->GetPositionX() - atEntry->x;
        float playerBoxDistY = player->GetPositionY() - atEntry->y;

        float rotPlayerX = float(atEntry->x + playerBoxDistX * cosVal - playerBoxDistY*sinVal);
        float rotPlayerY = float(atEntry->y + playerBoxDistY * cosVal + playerBoxDistX*sinVal);

        // box edges are parallel to coordiante axis, so we can treat every dimension independently :D
        float dz = player->GetPositionZ() - atEntry->z;
        float dx = rotPlayerX - atEntry->x;
        float dy = rotPlayerY - atEntry->y;
        if ((fabs(dx) > atEntry->box_x / 2 + delta) ||
            (fabs(dy) > atEntry->box_y / 2 + delta) ||
            (fabs(dz) > atEntry->box_z / 2 + delta))
        {
            sLog->outDebug(LOG_FILTER_NETWORKIO, "HandleAreaTriggerOpcode: Player '%s' (GUID: %u) too far (1/2 box X: %f 1/2 box Y: %f 1/2 box Z: %f rotatedPlayerX: %f rotatedPlayerY: %f dZ:%f), ignore Area Trigger ID: %u",
                player->GetName(), player->GetGUIDLow(), atEntry->box_x/2, atEntry->box_y/2, atEntry->box_z/2, rotPlayerX, rotPlayerY, dz, triggerId);
            return;
        }
    }

    if (player->isDebugAreaTriggers)
        ChatHandler(player).PSendSysMessage(LANG_DEBUG_AREATRIGGER_REACHED, triggerId);

    if (sScriptMgr->OnAreaTrigger(player, atEntry))
        return;

    if (player->isAlive())
        if (uint32 questId = sObjectMgr->GetQuestForAreaTrigger(triggerId))
            if (player->GetQuestStatus(questId) == QUEST_STATUS_INCOMPLETE)
                player->AreaExploredOrEventHappens(questId);

    if (sObjectMgr->IsTavernAreaTrigger(triggerId))
    {
        // set resting flag we are in the inn
        player->SetFlag(PLAYER_FIELD_PLAYER_FLAGS, PLAYER_FLAGS_RESTING);
        player->InnEnter(time(NULL), atEntry->mapid, atEntry->x, atEntry->y, atEntry->z);
        player->SetRestType(REST_TYPE_IN_TAVERN);

        if (sWorld->IsFFAPvPRealm())
            player->RemoveByteFlag(UNIT_FIELD_SHAPESHIFT_FORM, 1, UNIT_BYTE2_FLAG_FFA_PVP);

        return;
    }

    if (Battleground* bg = player->GetBattleground())
        if (bg->GetStatus() == STATUS_IN_PROGRESS)
        {
            bg->HandleAreaTrigger(player, triggerId);
            return;
        }

        if (OutdoorPvP* pvp = player->GetOutdoorPvP())
            if (pvp->HandleAreaTrigger(m_Player, triggerId))
                return;

        AreaTriggerStruct const* at = sObjectMgr->GetAreaTrigger(triggerId);
        if (!at)
            return;

        bool teleported = false;
        if (player->GetMapId() != at->target_mapId)
        {
            if (!sMapMgr->CanPlayerEnter(at->target_mapId, player, false))
                return;

            if (Group* group = player->GetGroup())
                if (group->isLFGGroup() && player->GetMap()->IsDungeon())
                    teleported = player->TeleportToBGEntryPoint();
        }

        if (!teleported)
            player->TeleportTo(at->target_mapId, at->target_X, at->target_Y, at->target_Z, at->target_Orientation, TELE_TO_NOT_LEAVE_TRANSPORT);
}

void WorldSession::HandleUpdateAccountData(WorldPacket& recvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Received CMSG_UPDATE_ACCOUNT_DATA");

    uint32 type, timestamp, decompressedSize, compressedSize;
    recvData >> decompressedSize >> timestamp >> compressedSize;

    type = uint8(recvData.contents()[recvData.size()-1]) >> 5;

    sLog->outDebug(LOG_FILTER_NETWORKIO, "UAD: type %u, time %u, decompressedSize %u", type, timestamp, decompressedSize);

    if (decompressedSize == 0)                               // erase
    {
        SetAccountData(AccountDataType(type), 0, "");
        return;
    }

    if (decompressedSize > 0xFFFF)
    {
        recvData.rfinish();                   // unnneded warning spam in this case
        sLog->outError(LOG_FILTER_NETWORKIO, "UAD: Account data packet too big, size %u", decompressedSize);
        return;
    }

    ByteBuffer dest;
    dest.resize(decompressedSize);

    uLongf realSize = decompressedSize;
    if (uncompress(const_cast<uint8*>(dest.contents()), &realSize, const_cast<uint8*>(recvData.contents() + recvData.rpos()), recvData.size() - recvData.rpos()) != Z_OK)
    {
        recvData.rfinish();                   // unnneded warning spam in this case
        sLog->outError(LOG_FILTER_NETWORKIO, "UAD: Failed to decompress account data");
        return;
    }

    recvData.rfinish();                       // uncompress read (recvData.size() - recvData.rpos())

    std::string adata = dest.ReadString(decompressedSize);

    SetAccountData(AccountDataType(type), timestamp, adata);
}

void WorldSession::HandleRequestAccountData(WorldPacket& p_Packet)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Received CMSG_REQUEST_ACCOUNT_DATA");

    uint64 l_CharacterGuid = 0;
    uint32 l_Type;

    p_Packet.readPackGUID(l_CharacterGuid);
    l_Type = p_Packet.ReadBits(3);

    sLog->outDebug(LOG_FILTER_NETWORKIO, "RAD: type %u", l_Type);

    if (l_Type > NUM_ACCOUNT_DATA_TYPES)
        return;

    AccountData* l_AccountData = GetAccountData(AccountDataType(l_Type));

    uint32 l_Size       = l_AccountData->Data.size();
    uLongf l_DestSize   = compressBound(l_Size);

    ByteBuffer l_CompressedData;
    l_CompressedData.resize(l_DestSize);

    if (l_Size && compress(const_cast<uint8*>(l_CompressedData.contents()), &l_DestSize, (uint8*)l_AccountData->Data.c_str(), l_Size) != Z_OK)
    {
        sLog->outDebug(LOG_FILTER_NETWORKIO, "RAD: Failed to compress account data");
        return;
    }

    l_CompressedData.resize(l_DestSize);

    WorldPacket l_Response(SMSG_UPDATE_ACCOUNT_DATA, 4+4+4+3+3+5+8+l_DestSize);

    l_Response.appendPackGUID(l_CharacterGuid);
    l_Response << uint32(l_AccountData->Time);      /// unix time
    l_Response << uint32(l_DestSize);               /// compressed length
    l_Response.WriteBits(l_Type, 3);
    l_Response.FlushBits();
    l_Response.append(l_CompressedData);            /// compressed data
    l_Response << uint32(l_Size);                   /// decompressed length

    SendPacket(&l_Response);
}

int32 WorldSession::HandleEnableNagleAlgorithm()
{
    // Instructs the server we wish to receive few amounts of large packets (SMSG_MULTIPLE_PACKETS?)
    // instead of large amount of small packets
    return 0;
}

void WorldSession::HandleSetActionButtonOpcode(WorldPacket& p_RecvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Received CMSG_SET_ACTION_BUTTON");

    uint64 l_PackedData = 0;

    uint8 l_Button = 0;

    p_RecvData >> l_PackedData;
    p_RecvData >> l_Button;

    uint8   l_Type      = ACTION_BUTTON_TYPE(l_PackedData);
    uint32  l_ActionID  = ACTION_BUTTON_ACTION(l_PackedData);

    sLog->outInfo(LOG_FILTER_NETWORKIO, "BUTTON: %u ACTION: %u TYPE: %u", l_Button, l_ActionID, l_Type);

    if (!l_PackedData)
    {
        sLog->outInfo(LOG_FILTER_NETWORKIO, "MISC: Remove action from button %u", l_Button);
        GetPlayer()->removeActionButton(l_Button);
    }
    else
    {
        GetPlayer()->addActionButton(l_Button, l_ActionID, l_Type);
    }
}

void WorldSession::HandleCompleteCinematic(WorldPacket& /*recvData*/)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Received CMSG_COMPLETE_CINEMATIC");

    m_Player->StopCinematic();
}

void WorldSession::HandleNextCinematicCamera(WorldPacket& /*recvData*/)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Received CMSG_NEXT_CINEMATIC_CAMERA");
}

void WorldSession::HandleMoveTimeSkippedOpcode(WorldPacket& recvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Received CMSG_MOVE_TIME_SKIPPED");

    ObjectGuid guid;
    uint32 time;
    recvData >> time;

    uint8 bitOrder[8] = { 3, 0, 5, 1, 7, 6, 4, 2 };
    recvData.ReadBitInOrder(guid, bitOrder);

    recvData.FlushBits();

    uint8 byteOrder[8] = { 1, 6, 0, 5, 3, 4, 2, 7 };
    recvData.ReadBytesSeq(guid, byteOrder);

    //TODO!

    /*
    uint64 guid;
    uint32 time_skipped;
    recvData >> guid;
    recvData >> time_skipped;
    sLog->outDebug(LOG_FILTER_PACKETIO, "WORLD: CMSG_MOVE_TIME_SKIPPED");

    /// TODO
    must be need use in Trinity
    We substract server Lags to move time (AntiLags)
    for exmaple
    GetPlayer()->ModifyLastMoveTime(-int32(time_skipped));
    */
}

void WorldSession::HandleFeatherFallAck(WorldPacket& recvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: CMSG_MOVE_FEATHER_FALL_ACK");

    // no used
    recvData.rfinish();                       // prevent warnings spam
}

void WorldSession::HandleMoveUnRootAck(WorldPacket& recvData)
{
    // no used
    recvData.rfinish();                       // prevent warnings spam
    /*
    uint64 guid;
    recvData >> guid;

    // now can skip not our packet
    if (_player->GetGUID() != guid)
    {
    recvData.rfinish();                   // prevent warnings spam
    return;
    }

    sLog->outDebug(LOG_FILTER_PACKETIO, "WORLD: CMSG_FORCE_MOVE_UNROOT_ACK");

    recvData.read_skip<uint32>();                          // unk

    MovementInfo movementInfo;
    movementInfo.guid = guid;
    ReadMovementInfo(recvData, &movementInfo);
    recvData.read_skip<float>();                           // unk2
    */
}

void WorldSession::HandleMoveRootAck(WorldPacket& recvData)
{
    // no used
    recvData.rfinish();                       // prevent warnings spam
    /*
    uint64 guid;
    recvData >> guid;

    // now can skip not our packet
    if (_player->GetGUID() != guid)
    {
    recvData.rfinish();                   // prevent warnings spam
    return;
    }

    sLog->outDebug(LOG_FILTER_PACKETIO, "WORLD: CMSG_FORCE_MOVE_ROOT_ACK");

    recvData.read_skip<uint32>();                          // unk

    MovementInfo movementInfo;
    ReadMovementInfo(recvData, &movementInfo);
    */
}

void WorldSession::HandleSetActionBarToggles(WorldPacket& recvData)
{
    uint8 actionBar;

    recvData >> actionBar;

    if (!GetPlayer())                                        // ignore until not logged (check needed because STATUS_AUTHED)
    {
        if (actionBar != 0)
            sLog->outError(LOG_FILTER_NETWORKIO, "WorldSession::HandleSetActionBarToggles in not logged state with value: %u, ignored", uint32(actionBar));
        return;
    }

    GetPlayer()->SetByteValue(PLAYER_FIELD_LIFETIME_MAX_RANK, 2, actionBar);
}

void WorldSession::HandlePlayedTime(WorldPacket& recvData)
{
    bool unk1 = recvData.ReadBit();                 // 0 or 1 expected

    WorldPacket data(SMSG_PLAYED_TIME, 4 + 4 + 1);
    data << uint32(m_Player->GetLevelPlayedTime());
    data << uint32(m_Player->GetTotalPlayedTime());
    data.WriteBit(unk1);                            // 0 - will not show in chat frame
    data.FlushBits();
    SendPacket(&data);
}

void WorldSession::HandleInspectOpcode(WorldPacket& recvData)
{
    ObjectGuid playerGuid;

    uint8 bitOrder[8] = { 2, 5, 1, 6, 7, 4, 0, 3 };
    recvData.ReadBitInOrder(playerGuid, bitOrder);

    recvData.FlushBits();

    uint8 byteOrder[8] = { 0, 1, 5, 6, 4, 2, 3, 7 };
    recvData.ReadBytesSeq(playerGuid, byteOrder);

    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Received CMSG_INSPECT");

    m_Player->SetSelection(playerGuid);

    Player* player = ObjectAccessor::FindPlayer(playerGuid);
    if (!player)
    {
        sLog->outDebug(LOG_FILTER_NETWORKIO, "CMSG_INSPECT: No player found from GUID: " UI64FMTD, uint64(playerGuid));
        return;
    }

    WorldPacket data(SMSG_INSPECT_TALENT);

    ByteBuffer talentData;
    ByteBuffer glyphData;

    uint32 talentCount = 0;
    uint32 glyphCount = 0;
    uint32 equipmentCount = 0;

    data.WriteBit(playerGuid[7]);
    data.WriteBit(playerGuid[3]);

    Guild* guild = sGuildMgr->GetGuildById(player->GetGuildId());
    data.WriteBit(guild != nullptr);

    if (guild != nullptr)
    {
        ObjectGuid guildGuid = guild->GetGUID();

        data.WriteBit(guildGuid[6]);
        data.WriteBit(guildGuid[7]);
        data.WriteBit(guildGuid[4]);
        data.WriteBit(guildGuid[5]);
        data.WriteBit(guildGuid[2]);
        data.WriteBit(guildGuid[3]);
        data.WriteBit(guildGuid[1]);
        data.WriteBit(guildGuid[0]);
    }

    for (auto itr : *player->GetTalentMap(player->GetActiveSpec()))
    {
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(itr.first);
        if (spell && spell->talentId)
        {
            talentData << uint16(spell->talentId);
            ++talentCount;
        }
    }

    for (uint8 i = 0; i < MAX_GLYPH_SLOT_INDEX; ++i)
    {
        if (player->GetGlyph(0, i) == 0)
            continue;

        glyphData << uint16(player->GetGlyph(0, i));               // GlyphProperties.dbc
        ++glyphCount;
    }

    data.WriteBits(talentCount, 23);
    data.WriteBits(glyphCount, 23);
    data.WriteBit(playerGuid[5]);
    data.WriteBit(playerGuid[2]);
    data.WriteBit(playerGuid[6]);

    for (uint32 i = 0; i < EQUIPMENT_SLOT_END; ++i)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i);
        if (!item)
            continue;

        ++equipmentCount;
    }

    data.WriteBits(equipmentCount, 20);

    for (uint32 i = 0; i < EQUIPMENT_SLOT_END; ++i)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i);
        if (!item)
            continue;

        ObjectGuid itemCreator = item->GetUInt64Value(ITEM_FIELD_CREATOR);

        data.WriteBit(itemCreator[0]);
        data.WriteBit(0);               // unk bit 32
        data.WriteBit(itemCreator[6]);
        data.WriteBit(itemCreator[7]);
        data.WriteBit(0);               // unk bit 16
        data.WriteBit(itemCreator[3]);
        data.WriteBit(itemCreator[2]);
        data.WriteBit(itemCreator[1]);

        uint32 enchantmentCount = 0;

        for (uint32 j = 0; j < MAX_ENCHANTMENT_SLOT; ++j)
        {
            uint32 enchId = item->GetEnchantmentId(EnchantmentSlot(j));
            if (!enchId)
                continue;

            ++enchantmentCount;
        }

        data.WriteBits(enchantmentCount, 21);
        data.WriteBit(0);               // unk bit
        data.WriteBit(itemCreator[5]);
        data.WriteBit(itemCreator[4]);
    }

    data.WriteBit(playerGuid[4]);
    data.WriteBit(playerGuid[1]);
    data.WriteBit(playerGuid[0]);

    for (uint32 i = 0; i < EQUIPMENT_SLOT_END; ++i)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i);
        if (!item)
            continue;

        ObjectGuid itemCreator = item->GetUInt64Value(ITEM_FIELD_CREATOR);

        // related to random stats
        // if (unkBit)
        //     data << uint16(UNK);

        data.WriteByteSeq(itemCreator[3]);
        data.WriteByteSeq(itemCreator[4]);

        for (uint32 j = 0; j < MAX_ENCHANTMENT_SLOT; ++j)
        {
            uint32 enchId = item->GetEnchantmentId(EnchantmentSlot(j));
            if (!enchId)
                continue;

            data << uint8(j);
            data << uint32(enchId);
        }

        // if (unkBit)
        //     data << uint32(UNK);

        data.WriteByteSeq(itemCreator[0]);
        data.WriteByteSeq(itemCreator[2]);
        data.WriteByteSeq(itemCreator[6]);

        uint32 mask = 0;
        uint32 modifiers = 0;
        if (item->GetDynamicUInt32Value(ITEM_DYNAMIC_MODIFIERS, 0))
        {
            ++modifiers;
            mask |= 0x1;
        }
        if (item->GetDynamicUInt32Value(ITEM_DYNAMIC_MODIFIERS, 1))
        {
            ++modifiers;
            mask |= 0x2;
        }
        if (item->GetDynamicUInt32Value(ITEM_DYNAMIC_MODIFIERS, 2))
        {
            ++modifiers;
            mask |= 0x4;
        }
        data << uint32(modifiers == 0 ? 0 : ((modifiers*4) + 4));
        if (modifiers > 0)
        {
            data << uint32(mask);
            if (uint32 reforge = item->GetDynamicUInt32Value(ITEM_DYNAMIC_MODIFIERS, 0))
                data << uint32(reforge);
            if (uint32 transmogrification = item->GetDynamicUInt32Value(ITEM_DYNAMIC_MODIFIERS, 1))
                data << uint32(transmogrification);
            if (uint32 upgrade = item->GetDynamicUInt32Value(ITEM_DYNAMIC_MODIFIERS, 2))
                data << uint32(upgrade);
        }

        data.WriteByteSeq(itemCreator[1]);
        data.WriteByteSeq(itemCreator[7]);
        data.WriteByteSeq(itemCreator[5]);
        data << uint8(i);
        data << uint32(item->GetEntry());
    }

    data.WriteByteSeq(playerGuid[7]);
    data.WriteByteSeq(playerGuid[1]);
    data.WriteByteSeq(playerGuid[5]);
    data.WriteByteSeq(playerGuid[0]);

    data << uint32(player->GetSpecializationId(player->GetActiveSpec()));

    if (guild != nullptr)
    {
        ObjectGuid guildGuid = guild->GetGUID();

        data << uint32(guild->GetLevel());

        data.WriteByteSeq(guildGuid[1]);
        data.WriteByteSeq(guildGuid[3]);

        data << uint32(guild->GetMembersCount());

        data.WriteByteSeq(guildGuid[6]);
        data.WriteByteSeq(guildGuid[2]);
        data.WriteByteSeq(guildGuid[5]);
        data.WriteByteSeq(guildGuid[4]);
        data.WriteByteSeq(guildGuid[0]);
        data.WriteByteSeq(guildGuid[7]);

        data << uint64(guild->GetExperience());
    }

    data.WriteByteSeq(playerGuid[6]);
    data.WriteByteSeq(playerGuid[4]);
    data.WriteByteSeq(playerGuid[2]);
    data.WriteByteSeq(playerGuid[3]);

    data.append(talentData);
    data.append(glyphData);

    SendPacket(&data);
}

void WorldSession::HandleInspectHonorStatsOpcode(WorldPacket& recvData)
{
    ObjectGuid guid;
    uint8 bitOrder[8] = { 0, 3, 2, 4, 6, 7, 5, 1 };
    recvData.ReadBitInOrder(guid, bitOrder);

    recvData.FlushBits();

    uint8 byteOrder[8] = { 5, 0, 2, 7, 6, 3, 1, 4 };
    recvData.ReadBytesSeq(guid, byteOrder);

    Player* player = ObjectAccessor::FindPlayer(guid);
    if (!player)
    {
        //sLog->outDebug(LOG_FILTER_NETWORKIO, "CMSG_INSPECT_HONOR_STATS: No player found from GUID: " UI64FMTD, guid);
        return;
    }

    ObjectGuid playerGuid = guid;
    WorldPacket data(SMSG_INSPECT_HONOR_STATS);

    uint8 bitOrder2[8] = { 5, 3, 7, 2, 1, 6, 0, 4 };
    data.WriteBitInOrder(playerGuid, bitOrder2);

    const uint32 cycleCount = 3; // MAX_ARENA_SLOT
    data.WriteBits(cycleCount, 3);

    data.WriteByteSeq(playerGuid[7]);

    for (int i = 0; i < cycleCount; ++i)
    {
        // Client display only this two fields

        data << uint32(player->GetSeasonWins(i));
        data << uint32(0);
        data << uint32(0);

        data << uint8(i);

        data << uint32(0);
        data << uint32(0);
        data << uint32(player->GetArenaPersonalRating(i));
        data << uint32(0);
    }

    data.WriteByteSeq(playerGuid[1]);
    data.WriteByteSeq(playerGuid[5]);
    data.WriteByteSeq(playerGuid[0]);
    data.WriteByteSeq(playerGuid[3]);
    data.WriteByteSeq(playerGuid[2]);
    data.WriteByteSeq(playerGuid[6]);
    data.WriteByteSeq(playerGuid[4]);

    SendPacket(&data);
}

void WorldSession::HandleInspectRatedBGStatsOpcode(WorldPacket& recvData)
{
    uint32 unk;
    ObjectGuid guid;

    recvData >> unk;

    uint8 bitOrder[8] = { 1, 3, 5, 2, 6, 7, 0, 4 };
    recvData.ReadBitInOrder(guid, bitOrder);

    recvData.FlushBits();

    uint8 byteOrder[8] = { 4, 7, 0, 3, 5, 2, 6, 1 };
    recvData.ReadBytesSeq(guid, byteOrder);

    Player* player = ObjectAccessor::FindPlayer(guid);
    if (!player)
    {
        //sLog->outDebug(LOG_FILTER_NETWORKIO, "CMSG_REQUEST_INSPECT_RATED_BG_STATS: No player found from GUID: " UI64FMTD, guid);
        return;
    }

    return;

    // TODO //
    ObjectGuid playerGuid = player->GetGUID();
    WorldPacket data(SMSG_INSPECT_RATED_BG_STATS);

    ObjectGuid gguid = guid;
    data << uint32(0); //SeasonWin
    data << uint32(0); //SeasonPlayed
    data << uint32(0); //Rating

    uint8 bitOrder3[8] = {5, 7, 2, 3, 4, 6, 0, 1};
    data.WriteBitInOrder(gguid, bitOrder3);

    uint8 byteOrder2[8] = {6, 2, 3, 1, 7, 5, 4, 0};
    data.WriteBytesSeq(gguid, byteOrder2);

    SendPacket(&data);
}

void WorldSession::HandleWorldTeleportOpcode(WorldPacket& recvData)
{
    uint32 time;
    uint32 mapid;
    float PositionX;
    float PositionY;
    float PositionZ;
    float Orientation;

    recvData >> time;                                      // time in m.sec.
    recvData >> mapid;
    recvData >> PositionX;
    recvData >> Orientation;
    recvData >> PositionY;
    recvData >> PositionZ;                          // o (3.141593 = 180 degrees)

    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: Received CMSG_WORLD_TELEPORT");

    if (GetPlayer()->isInFlight())
    {
        sLog->outDebug(LOG_FILTER_NETWORKIO, "Player '%s' (GUID: %u) in flight, ignore worldport command.", GetPlayer()->GetName(), GetPlayer()->GetGUIDLow());
        return;
    }

    sLog->outDebug(LOG_FILTER_NETWORKIO, "CMSG_WORLD_TELEPORT: Player = %s, Time = %u, map = %u, x = %f, y = %f, z = %f, o = %f", GetPlayer()->GetName(), time, mapid, PositionX, PositionY, PositionZ, Orientation);

    if (AccountMgr::IsAdminAccount(GetSecurity()))
        GetPlayer()->TeleportTo(mapid, PositionX, PositionY, PositionZ, Orientation);
    else
        SendNotification(LANG_YOU_NOT_HAVE_PERMISSION);
}

void WorldSession::HandleWhoisOpcode(WorldPacket& recvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "Received opcode CMSG_WHOIS");

    uint32 textLength = recvData.ReadBits(6);
    recvData.FlushBits();
    std::string charname = recvData.ReadString(textLength);

    if (!AccountMgr::IsAdminAccount(GetSecurity()))
    {
        SendNotification(LANG_YOU_NOT_HAVE_PERMISSION);
        return;
    }

    if (charname.empty() || !normalizePlayerName (charname))
    {
        SendNotification(LANG_NEED_CHARACTER_NAME);
        return;
    }

    Player* player = sObjectAccessor->FindPlayerByName(charname.c_str());

    if (!player)
    {
        SendNotification(LANG_PLAYER_NOT_EXIST_OR_OFFLINE, charname.c_str());
        return;
    }

    uint32 accid = player->GetSession()->GetAccountId();

    PreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_ACCOUNT_WHOIS);

    stmt->setUInt32(0, accid);

    PreparedQueryResult result = LoginDatabase.Query(stmt);

    if (!result)
    {
        SendNotification(LANG_ACCOUNT_FOR_PLAYER_NOT_FOUND, charname.c_str());
        return;
    }

    Field* fields = result->Fetch();
    std::string acc = fields[0].GetString();
    if (acc.empty())
        acc = "Unknown";
    std::string email = fields[1].GetString();
    if (email.empty())
        email = "Unknown";
    std::string lastip = fields[2].GetString();
    if (lastip.empty())
        lastip = "Unknown";

    std::string msg = charname + "'s " + "account is " + acc + ", e-mail: " + email + ", last ip: " + lastip;

    WorldPacket data(SMSG_WHOIS, msg.size()+1);
    data.WriteBits(msg.size(), 11);

    data.FlushBits();
    if (msg.size())
        data.append(msg.c_str(), msg.size());

    SendPacket(&data);

    sLog->outDebug(LOG_FILTER_NETWORKIO, "Received whois command from player %s for character %s", GetPlayer()->GetName(), charname.c_str());
}

void WorldSession::HandleComplainOpcode(WorldPacket& recvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: CMSG_COMPLAIN");

    // recvData is not empty, but all data are unused in core
    // NOTE: all chat messages from this spammer automatically ignored by spam reporter until logout in case chat spam.
    // if it's mail spam - ALL mails from this spammer automatically removed by client

    // Complaint Received message
    WorldPacket data(SMSG_COMPLAIN_RESULT, 2);
    data << uint8(0);   // value 1 resets CGChat::m_complaintsSystemStatus in client. (unused?)
    data << uint32(0);  // value 0xC generates a "CalendarError" in client.
    SendPacket(&data);
}

void WorldSession::HandleRealmSplitOpcode(WorldPacket& recvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "CMSG_REALM_SPLIT");

    uint32 unk;
    std::string split_date = "01/01/01";
    recvData >> unk;

    WorldPacket data(SMSG_REALM_SPLIT);
    data.WriteBits(split_date.size(), 7);
    data << unk;
    data << uint32(0x00000000);                             // realm split state
    data << split_date;
    SendPacket(&data);
}

enum RealmQueryNameResponse
{
    REALM_QUERY_NAME_RESPONSE_OK        = 0,
    REALM_QUERY_NAME_RESPONSE_DENY      = 1,
    REALM_QUERY_NAME_RESPONSE_RETRY     = 2,
    REALM_QUERY_NAME_RESPONSE_OK_TEMP   = 3,
};

void WorldSession::HandleRealmQueryNameOpcode(WorldPacket& recvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "CMSG_REALM_QUERY_NAME");

    uint32 realmId = recvData.read<uint32>();

    if (realmId != realmID)
        return; // Cheater ?

    std::string realmName = sWorld->GetRealmName();

    WorldPacket data(SMSG_REALM_QUERY_RESPONSE);
    data << realmID;
    data << uint8(REALM_QUERY_NAME_RESPONSE_OK);
    data.WriteBit(1); // Is Locale
    data.WriteBits(realmName.size(), 8);
    data.WriteBits(realmName.size(), 8);
    data.FlushBits();
    data.append(realmName.c_str(), realmName.size());
    data.append(realmName.c_str(), realmName.size());

    SendPacket(&data);
}

void WorldSession::HandleFarSightOpcode(WorldPacket& recvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: CMSG_FAR_SIGHT");

    bool apply = recvData.ReadBit();

    if (apply)
    {
        sLog->outDebug(LOG_FILTER_NETWORKIO, "Player %u set vision to self", m_Player->GetGUIDLow());
        m_Player->SetSeer(m_Player);
    }
    else
    {
        sLog->outDebug(LOG_FILTER_NETWORKIO, "Added FarSight " UI64FMTD " to player %u", m_Player->GetUInt64Value(PLAYER_FIELD_FARSIGHT_OBJECT), m_Player->GetGUIDLow());
        if (WorldObject* target = m_Player->GetViewpoint())
            m_Player->SetSeer(target);
        else
            sLog->outError(LOG_FILTER_NETWORKIO, "Player %s requests non-existing seer " UI64FMTD, m_Player->GetName(), m_Player->GetUInt64Value(PLAYER_FIELD_FARSIGHT_OBJECT));
    }

    GetPlayer()->UpdateVisibilityForPlayer();
}

void WorldSession::HandleSetTitleOpcode(WorldPacket& p_Packet)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "CMSG_SET_TITLE");

    uint32 l_Title 0;

    p_Packet >> l_Title;

    /// -1 at none
    if (l_Title > 0 && l_Title < MAX_TITLE_INDEX)
    {
        if (!GetPlayer()->HasTitle(l_Title))
            return;
    }
    else
        l_Title = 0;

    GetPlayer()->SetUInt32Value(PLAYER_FIELD_PLAYER_TITLE, l_Title);
}

void WorldSession::HandleTimeSyncResp(WorldPacket& recvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "CMSG_TIME_SYNC_RESP");

    uint32 counter, clientTicks;
    recvData >> counter >> clientTicks;

    if (counter != m_Player->m_timeSyncCounter - 1)
        sLog->outDebug(LOG_FILTER_NETWORKIO, "Wrong time sync counter from player %s (cheater?)", m_Player->GetName());

    sLog->outDebug(LOG_FILTER_NETWORKIO, "Time sync received: counter %u, client ticks %u, time since last sync %u", counter, clientTicks, clientTicks - m_Player->m_timeSyncClient);

    uint32 ourTicks = clientTicks + (getMSTime() - m_Player->m_timeSyncServer);

    // diff should be small
    sLog->outDebug(LOG_FILTER_NETWORKIO, "Our ticks: %u, diff %u, latency %u", ourTicks, ourTicks - clientTicks, GetLatency());

    m_Player->m_timeSyncClient = clientTicks;
}

void WorldSession::HandleResetInstancesOpcode(WorldPacket& /*recvData*/)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: CMSG_RESET_INSTANCES");

    if (Group* group = m_Player->GetGroup())
    {
        if (group->IsLeader(m_Player->GetGUID()))
            group->ResetInstances(INSTANCE_RESET_ALL, false, m_Player);
    }
    else
        m_Player->ResetInstances(INSTANCE_RESET_ALL, false);
}

void WorldSession::HandleResetChallengeModeOpcode(WorldPacket& /*recvData*/)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: CMSG_RESET_CHALLENGE_MODE");

    // @TODO: Do something about challenge mode ...
}

void WorldSession::HandleSetDungeonDifficultyOpcode(WorldPacket & recvData)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "MSG_SET_DUNGEON_DIFFICULTY");

    uint32 mode;
    recvData >> mode;

    if (mode != CHALLENGE_MODE_DIFFICULTY && mode >= MAX_DUNGEON_DIFFICULTY)
    {
        sLog->outError(LOG_FILTER_NETWORKIO, "WorldSession::HandleSetDungeonDifficultyOpcode: player %d sent an invalid instance mode %d!", m_Player->GetGUIDLow(), mode);
        return;
    }

    if (Difficulty(mode) == m_Player->GetDungeonDifficulty())
        return;

    // cannot reset while in an instance
    Map* map = m_Player->FindMap();
    if (map && map->IsDungeon())
    {
        sLog->outError(LOG_FILTER_NETWORKIO, "WorldSession::HandleSetDungeonDifficultyOpcode: player (Name: %s, GUID: %u) tried to reset the instance while player is inside!", m_Player->GetName(), m_Player->GetGUIDLow());
        return;
    }

    Group* group = m_Player->GetGroup();
    if (group)
    {
        if (group->IsLeader(m_Player->GetGUID()))
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
            {
                Player* groupGuy = itr->getSource();
                if (!groupGuy)
                    continue;

                if (!groupGuy->IsInMap(groupGuy))
                    return;

                if (groupGuy->GetMap()->IsNonRaidDungeon())
                {
                    sLog->outError(LOG_FILTER_NETWORKIO, "WorldSession::HandleSetDungeonDifficultyOpcode: player %d tried to reset the instance while group member (Name: %s, GUID: %u) is inside!", m_Player->GetGUIDLow(), groupGuy->GetName(), groupGuy->GetGUIDLow());
                    return;
                }
            }
            // the difficulty is set even if the instances can't be reset
            //_player->SendDungeonDifficulty(true);
            group->ResetInstances(INSTANCE_RESET_CHANGE_DIFFICULTY, false, m_Player);
            group->SetDungeonDifficulty(Difficulty(mode));
            m_Player->SendDungeonDifficulty(true);
        }
    }
    else
    {
        m_Player->ResetInstances(INSTANCE_RESET_CHANGE_DIFFICULTY, false);
        m_Player->SetDungeonDifficulty(Difficulty(mode));
        m_Player->SendDungeonDifficulty(false);
    }
}

void WorldSession::HandleSetRaidDifficultyOpcode(WorldPacket& p_RecvData)
{
    uint32 l_Difficulty;
    uint8 l_IsLegacyDifficulty;

    p_RecvData >> l_Difficulty;
    p_RecvData >> l_IsLegacyDifficulty;

    if (!l_IsLegacyDifficulty && (l_Difficulty < NORMAL_DIFFICULTY || l_Difficulty > MYTHIC_DIFFICULTY))
    {
        sLog->outError(LOG_FILTER_NETWORKIO, "WorldSession::HandleSetRaidDifficultyOpcode: player %d sent an invalid instance mode %d!", m_Player->GetGUIDLow(), l_Difficulty);
        return;
    }

    if (l_IsLegacyDifficulty && (l_Difficulty < LEGACY_MAN10_DIFFICULTY || l_Difficulty > LEGACY_MAN25_HEROIC_DIFFICULTY))
    {
        sLog->outError(LOG_FILTER_NETWORKIO, "WorldSession::HandleSetRaidDifficultyOpcode: player %d sent an invalid instance mode %d!", m_Player->GetGUIDLow(), l_Difficulty);
        return;
    }

    // cannot reset while in an instance
    Map* l_Map = m_Player->FindMap();
    if (l_Map && l_Map->IsDungeon())
    {
        sLog->outError(LOG_FILTER_NETWORKIO, "WorldSession::HandleSetRaidDifficultyOpcode: player %d tried to reset the instance while inside!", m_Player->GetGUIDLow());
        return;
    }

    if (!l_IsLegacyDifficulty && Difficulty(l_Difficulty) == m_Player->GetRaidDifficulty())
        return;

    if (l_IsLegacyDifficulty && Difficulty(l_Difficulty) == m_Player->GetLegacyRaidDifficulty())
        return;

    Group* group = m_Player->GetGroup();
    if (group)
    {
        if (group->IsLeader(m_Player->GetGUID()))
        {
            for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
            {
                Player* groupGuy = itr->getSource();
                if (!groupGuy)
                    continue;

                if (!groupGuy->IsInMap(groupGuy))
                    return;

                if (groupGuy->GetMap()->IsRaid())
                {
                    sLog->outError(LOG_FILTER_NETWORKIO, "WorldSession::HandleSetRaidDifficultyOpcode: player %d tried to reset the instance while inside!", m_Player->GetGUIDLow());
                    return;
                }
            }
            // the difficulty is set even if the instances can't be reset
            //_player->SendDungeonDifficulty(true);
            group->ResetInstances(INSTANCE_RESET_CHANGE_DIFFICULTY, true, m_Player);

            if (l_IsLegacyDifficulty)
                group->SetLegacyRaidDifficulty(Difficulty(l_Difficulty));
            else
                group->SetRaidDifficulty(Difficulty(l_Difficulty));

            m_Player->SendRaidDifficulty(true);
        }
    }
    else
    {
        m_Player->ResetInstances(INSTANCE_RESET_CHANGE_DIFFICULTY, true);

        if (l_IsLegacyDifficulty)
            m_Player->SetLegacyRaidDifficulty(Difficulty(l_Difficulty));
        else
            m_Player->SetRaidDifficulty(Difficulty(l_Difficulty));

        m_Player->SendRaidDifficulty(false);
    }
}

void WorldSession::HandleCancelMountAuraOpcode(WorldPacket& /*recvData*/)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: CMSG_CANCEL_MOUNT_AURA");

    //If player is not mounted, so go out :)
    if (!m_Player->IsMounted())                              // not blizz like; no any messages on blizz
    {
        ChatHandler(this).SendSysMessage(LANG_CHAR_NON_MOUNTED);
        return;
    }

    if (m_Player->isInFlight())                               // not blizz like; no any messages on blizz
    {
        ChatHandler(this).SendSysMessage(LANG_YOU_IN_FLIGHT);
        return;
    }

    m_Player->Dismount();
    m_Player->RemoveAurasByType(SPELL_AURA_MOUNTED);
}

void WorldSession::HandleRequestPetInfoOpcode(WorldPacket& /*recvData */)
{
    /*
    sLog->outDebug(LOG_FILTER_PACKETIO, "WORLD: CMSG_REQUEST_PET_INFO");
    recvData.hexlike();
    */
}

void WorldSession::HandleSetTaxiBenchmarkOpcode(WorldPacket& recvData)
{
    bool mode = recvData.ReadBit();

    sLog->outDebug(LOG_FILTER_NETWORKIO, "Client used \"/timetest %d\" command", mode);
}

void WorldSession::HandleQueryInspectAchievements(WorldPacket& recvData)
{
    ObjectGuid guid;

    uint8 bitsOrder[8] = { 7, 4, 0, 5, 2, 1, 3, 6 };
    recvData.ReadBitInOrder(guid, bitsOrder);

    recvData.FlushBits();

    uint8 bytesOrder[8] = { 3, 2, 4, 6, 1, 7, 5, 0 };
    recvData.ReadBytesSeq(guid, bytesOrder);

    Player* player = ObjectAccessor::FindPlayer(guid);
    if (!player)
        return;

    player->GetAchievementMgr().SendAchievementInfo(m_Player);
}

void WorldSession::HandleGuildAchievementProgressQuery(WorldPacket& recvData)
{
    uint32 achievementId;
    recvData >> achievementId;

    if (Guild* guild = sGuildMgr->GetGuildById(m_Player->GetGuildId()))
        guild->GetAchievementMgr().SendAchievementInfo(m_Player, achievementId);
}

void WorldSession::HandleWorldStateUITimerUpdate(WorldPacket& /*recvData*/)
{
    // empty opcode
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: CMSG_WORLD_STATE_UI_TIMER_UPDATE");

    WorldPacket data(SMSG_WORLD_STATE_UI_TIMER_UPDATE, 4);
    data << uint32(time(NULL));
    SendPacket(&data);
}

void WorldSession::HandleReadyForAccountDataTimes(WorldPacket& /*recvData*/)
{
    // empty opcode
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: CMSG_READY_FOR_ACCOUNT_DATA_TIMES");

    SendAccountDataTimes(GLOBAL_CACHE_MASK);
}

void WorldSession::SendSetPhaseShift(std::set<uint32> const& phaseIds, std::set<uint32> const& terrainswaps)
{
    ObjectGuid guid = m_Player->GetGUID();
    uint32 unkValue = 0;
    uint32 inactiveSwapsCount = 0;

    /* 5.4.0 sniff data
    18000000    // uint32 0x18
    00000000    // uint32 unk value
    04000000    // 4 (2) counter
    EC02        // 748	Cosmetic - Krasarang Wilds (Horde)
    8E03        // 910	Cosmetic - Fort Silverback (Prelude 002)
    1A000000 // 26 (13) counter, look line inactiveSwap
    6D88
    7688
    8388
    8488
    9E88
    AB88
    AC88
    B888
    C588
    C788
    C988
    DB88
    DD88
    04000000
    60 04 // 1121 (mapid)
    61 04 // 1030 (mapid)

    ED // guid bitmask
    79 07 81 23 56 02 // guid bytes*/

    WorldPacket data(SMSG_SET_PHASE_SHIFT, 1 + 8 + 4 + 4 + 4 + 4 + 2 * phaseIds.size() + 4 + terrainswaps.size() * 2);

    // 0x8 or 0x10 is related to areatrigger, if we send flags 0x00 areatrigger doesn't work in some case
    data << uint32(0x18); // flags, 0x18 most of time on retail sniff

    // Active terrain swaps, may switch with inactive terrain
    data << uint32(terrainswaps.size() * 2);
    for (std::set<uint32>::const_iterator itr = terrainswaps.begin(); itr != terrainswaps.end(); ++itr)
        data << uint16(*itr);

    // Inactive terrain swaps, may switch with active terrain
    data << inactiveSwapsCount;
    //for (uint8 i = 0; i < inactiveSwapsCount; ++i)
        //data << uint16(0);

    data << uint32(phaseIds.size() * 2);        // Phase.dbc ids
    for (std::set<uint32>::const_iterator itr = phaseIds.begin(); itr != phaseIds.end(); ++itr)
        data << uint16(*itr); // Most of phase id on retail sniff have 0x8000 mask

    // WorldMapAreaId ?
    data << unkValue;
    //for (uint32 i = 0; i < unkValue; i++)
        //data << uint16(0);

    uint8 bitOrder[8] = { 0, 2, 1, 5, 3, 7, 4, 6 };
    data.WriteBitInOrder(guid, bitOrder);

    uint8 byteOrder[8] = { 0, 5, 4, 7, 6, 2, 1, 3 };
    data.WriteBytesSeq(guid, byteOrder);

    SendPacket(&data);
}

// Battlefield and Battleground
void WorldSession::HandleAreaSpiritHealerQueryOpcode(WorldPacket& p_Packet)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: CMSG_AREA_SPIRIT_HEALER_QUERY");

    Battleground* l_Battleground = m_Player->GetBattleground();

    uint64 l_Healer = 0;

    p_Packet.readPackGUID(l_Healer);

    Creature * l_Unit = GetPlayer()->GetMap()->GetCreature(l_Healer);

    if (!l_Unit)
        return;

    /// It's not spirit service
    if (!l_Unit->isSpiritService())
        return;

    if (l_Battleground)
        sBattlegroundMgr->SendAreaSpiritHealerQueryOpcode(m_Player, l_Battleground, l_Healer);

    if (Battlefield * l_Battlefield = sBattlefieldMgr->GetBattlefieldToZoneId(m_Player->GetZoneId()))
        l_Battlefield->SendAreaSpiritHealerQueryOpcode(m_Player,l_Healer);
}

void WorldSession::HandleAreaSpiritHealerQueueOpcode(WorldPacket& p_Packet)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: CMSG_AREA_SPIRIT_HEALER_QUEUE");

    Battleground* l_Battleground = m_Player->GetBattleground();
    uint64 l_Healer = 0;

    p_Packet.readPackGUID(l_Healer);

    Creature* l_Unit = GetPlayer()->GetMap()->GetCreature(l_Healer);

    if (!l_Unit)
        return;

    /// it's not spirit service
    if (!l_Unit->isSpiritService())
        return;

    if (l_Battleground)
        l_Battleground->AddPlayerToResurrectQueue(l_Healer, m_Player->GetGUID());

    if (Battlefield * l_Battlefield = sBattlefieldMgr->GetBattlefieldToZoneId(m_Player->GetZoneId()))
        l_Battlefield->AddPlayerToResurrectQueue(l_Healer, m_Player->GetGUID());
}

void WorldSession::HandleHearthAndResurrect(WorldPacket& /*recvData*/)
{
    if (m_Player->isInFlight())
        return;

    if (/*Battlefield* bf =*/ sBattlefieldMgr->GetBattlefieldToZoneId(m_Player->GetZoneId()))
    {
        // bf->PlayerAskToLeave(_player);                   //@todo: FIXME
        return;
    }

    AreaTableEntry const* atEntry = GetAreaEntryByAreaID(m_Player->GetAreaId());
    if (!atEntry || !(atEntry->flags & AREA_FLAG_WINTERGRASP_2))
        return;

    m_Player->BuildPlayerRepop();
    m_Player->ResurrectPlayer(100);
    m_Player->TeleportTo(m_Player->m_homebindMapId, m_Player->m_homebindX, m_Player->m_homebindY, m_Player->m_homebindZ, m_Player->GetOrientation());
}

void WorldSession::HandleInstanceLockResponse(WorldPacket& recvPacket)
{
    uint8 accept;
    recvPacket >> accept;

    if (!m_Player->HasPendingBind())
    {
        sLog->outInfo(LOG_FILTER_NETWORKIO, "InstanceLockResponse: Player %s (guid %u) tried to bind himself/teleport to graveyard without a pending bind!", m_Player->GetName(), m_Player->GetGUIDLow());
        return;
    }

    if (accept)
        m_Player->BindToInstance();
    else
        m_Player->RepopAtGraveyard();

    m_Player->SetPendingBind(0, 0);
}

void WorldSession::HandleRequestHotfix(WorldPacket& p_RecvPacket)
{
    uint32 l_Type   = 0;
    uint32 l_Count  = 0;

    p_RecvPacket >> l_Type;
    p_RecvPacket >> l_Count;

    uint64 * l_Guids = new uint64[l_Count];

    for (uint32 l_I = 0; l_I < l_Count; ++l_I)
    {
        uint32 l_Entry;

        p_RecvPacket.readPackGUID(l_Guids[l_I]);
        p_RecvPacket >> l_Entry;

        switch (l_Type)
        {
            case DB2_REPLY_ITEM:
                SendItemDb2Reply(l_Entry);
                break;

            case DB2_REPLY_SPARSE:
#pragma message("DB2_REPLY_SPARSE TODO")
                ;// SendItemSparseDb2Reply(l_Entry);
                break;

            case DB2_REPLY_BROADCAST_TEXT:
                SendBroadcastTextDb2Reply(l_Entry);
                break;

            // TODO
            case DB2_REPLY_BATTLE_PET_EFFECT_PROPERTIES:
            case DB2_REPLY_SCENE_SCRIPT:
                break;

            default:
                break;
        }
    }

    delete[] l_Guids;
}

void WorldSession::HandleUpdateMissileTrajectory(WorldPacket& recvPacket)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "WORLD: CMSG_UPDATE_MISSILE_TRAJECTORY");

    uint64 guid;
    uint32 spellId;
    float elevation, speed;
    float curX, curY, curZ;
    float targetX, targetY, targetZ;
    uint8 moveStop;

    recvPacket >> guid >> spellId >> elevation >> speed;
    recvPacket >> curX >> curY >> curZ;
    recvPacket >> targetX >> targetY >> targetZ;
    recvPacket >> moveStop;

    Unit* caster = ObjectAccessor::GetUnit(*m_Player, guid);
    Spell* spell = caster ? caster->GetCurrentSpell(CURRENT_GENERIC_SPELL) : NULL;
    if (!spell || spell->m_spellInfo->Id != spellId || !spell->m_targets.HasDst() || !spell->m_targets.HasSrc())
    {
        recvPacket.rfinish();
        return;
    }

    Position pos = *spell->m_targets.GetSrcPos();
    pos.Relocate(curX, curY, curZ);
    spell->m_targets.ModSrc(pos);

    pos = *spell->m_targets.GetDstPos();
    pos.Relocate(targetX, targetY, targetZ);
    spell->m_targets.ModDst(pos);

    spell->m_targets.SetElevation(elevation);
    spell->m_targets.SetSpeed(speed);

    if (moveStop)
    {
        uint32 opcode;
        recvPacket >> opcode;
        recvPacket.SetOpcode(CMSG_MOVE_STOP); // always set to MSG_MOVE_STOP in client SetOpcode
        HandleMovementOpcodes(recvPacket);
    }
}

void WorldSession::HandleViolenceLevel(WorldPacket& recvPacket)
{
    uint8 violenceLevel;
    recvPacket >> violenceLevel;

    // do something?
}

void WorldSession::HandleObjectUpdateFailedOpcode(WorldPacket& recvPacket)
{
    ObjectGuid guid;

    uint8 bitOrder[8] = {2, 1, 0, 6, 3, 7, 4, 5};
    recvPacket.ReadBitInOrder(guid, bitOrder);

    uint8 byteOrder[8] = {7, 5, 1, 6, 4, 2, 3, 0};
    recvPacket.ReadBytesSeq(guid, byteOrder);

    WorldObject* obj = ObjectAccessor::GetWorldObject(*GetPlayer(), guid);
    if (obj)
        obj->SendUpdateToPlayer(GetPlayer());

    sLog->outError(LOG_FILTER_NETWORKIO, "Object update failed for object " UI64FMTD " (%s) for player %s (%u)", uint64(guid), obj ? obj->GetName() : "object-not-found", GetPlayerName().c_str(), GetGuidLow());
}

// DestrinyFrame.xml : lua function NeutralPlayerSelectFaction
#define JOIN_THE_ALLIANCE 1
#define JOIN_THE_HORDE    0

void WorldSession::HandleSetFactionOpcode(WorldPacket& recvPacket)
{
    uint32 choice = recvPacket.read<uint32>();

    if (m_Player->getRace() != RACE_PANDAREN_NEUTRAL)
        return;

    if (choice == JOIN_THE_HORDE)
    {
        m_Player->SetByteValue(UNIT_FIELD_SEX, 0, RACE_PANDAREN_HORDE);
        m_Player->setFactionForRace(RACE_PANDAREN_HORDE);
        m_Player->SaveToDB();
        WorldLocation location(1, 1366.730f, -4371.248f, 26.070f, 3.1266f);
        m_Player->TeleportTo(location);
        m_Player->SetHomebind(location, 363);
        m_Player->learnSpell(669, false); // Language Orcish
        m_Player->learnSpell(108127, false); // Language Pandaren
    }
    else if (choice == JOIN_THE_ALLIANCE)
    {
        m_Player->SetByteValue(UNIT_FIELD_SEX, 0, RACE_PANDAREN_ALLI);
        m_Player->setFactionForRace(RACE_PANDAREN_ALLI);
        m_Player->SaveToDB();
        WorldLocation location(0, -9096.236f, 411.380f, 92.257f, 3.649f);
        m_Player->TeleportTo(location);
        m_Player->SetHomebind(location, 9);
        m_Player->learnSpell(668, false); // Language Common
        m_Player->learnSpell(108127, false); // Language Pandaren
    }

    if (m_Player->GetQuestStatus(31450) == QUEST_STATUS_INCOMPLETE)
        m_Player->KilledMonsterCredit(64594);

    m_Player->SendMovieStart(116);
}

void WorldSession::HandleCategoryCooldownOpcode(WorldPacket& recvPacket)
{
    Unit::AuraEffectList const& list = GetPlayer()->GetAuraEffectsByType(SPELL_AURA_MOD_SPELL_CATEGORY_COOLDOWN);

    WorldPacket data(SMSG_CATEGORY_COOLDOWN, 4 + (int(list.size()) * 8));
    data.WriteBits<int>(list.size(), 21);
    for (Unit::AuraEffectList::const_iterator itr = list.begin(); itr != list.end(); ++itr)
    {
        AuraEffectPtr effect = *itr;
        if (!effect)
            continue;

        data << uint32(effect->GetMiscValue());
        data << int32(-effect->GetAmount());
    }

    SendPacket(&data);
}

void WorldSession::HandleTradeInfo(WorldPacket& recvPacket)
{
    uint8 bitOrder[8] = { 5, 4, 7, 1, 3, 6, 0, 2 };
    uint8 byteOrder[8] = { 7, 3, 4, 6, 1, 5, 0, 2 };

    uint32 skillId = recvPacket.read<uint32>();
    uint32 spellId = recvPacket.read<uint32>();

    ObjectGuid guid;

    recvPacket.ReadBitInOrder(guid, bitOrder);
    recvPacket.FlushBits();
    recvPacket.ReadBytesSeq(guid, byteOrder);

    Player* plr = sObjectAccessor->FindPlayer(guid);
    if (!plr || !plr->HasSkill(skillId) || !plr->HasSpell(spellId))
        return;

    uint32 spellSkillCount = 0;
    ByteBuffer buff(sizeof(uint32)*32);
    for (auto itr : plr->GetSpellMap())
    {
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(itr.first);
        if (!spell)
            continue;

        if (!spell->IsAbilityOfSkillType(skillId))
            continue;

        if (!(spell->Attributes & SPELL_ATTR0_TRADESPELL))
            continue;

        buff.append(itr.first);
        ++spellSkillCount;
    }
    WorldPacket data(SMSG_TRADE_INFO);
    data.WriteBit(guid[2]);
    data.WriteBit(guid[6]);
    data.WriteBit(guid[7]);
    data.WriteBits(spellSkillCount, 22);
    data.WriteBit(guid[5]);
    data.WriteBit(guid[1]);
    data.WriteBit(guid[4]);
    data.WriteBits(1, 22); // skill value count
    data.WriteBits(1, 22); // skill id count
    data.WriteBits(1, 22); // skill max value
    data.WriteBit(guid[3]);
    data.WriteBit(guid[0]);
    data.FlushBits();

    data << uint32(plr->GetSkillValue(skillId));
    data.WriteByteSeq(guid[0]);
    data << uint32(skillId);
    data.WriteByteSeq(guid[1]);
    data << uint32(spellId);

    data.append(buff);

    data.WriteByteSeq(guid[3]);
    data.WriteByteSeq(guid[5]);
    data.WriteByteSeq(guid[6]);
    data.WriteByteSeq(guid[4]);
    data.WriteByteSeq(guid[7]);
    data.WriteByteSeq(guid[2]);
    data << uint32(plr->GetMaxSkillValue(skillId));
    SendPacket(&data);
}

void WorldSession::HandleSaveCUFProfiles(WorldPacket& p_RecvPacket)
{
    uint32 l_ProfileCount;

    p_RecvPacket >> l_ProfileCount;

    if (l_ProfileCount > MAX_CUF_PROFILES)
    {
        p_RecvPacket.rfinish();
        return;
    }

    CUFProfiles& l_Profiles = GetPlayer()->m_cufProfiles;
    l_Profiles.resize(l_ProfileCount);

    for (uint32 l_I = 0; l_I < l_ProfileCount; ++l_I)
    {
        CUFProfile& l_Profile = l_Profiles[l_I];
        CUFProfileData& l_ProfileData = l_Profile.data;

        l_Profile.l_NameLen = p_RecvPacket.ReadBits(7);

        if (l_Profile.l_NameLen > MAX_CUF_PROFILE_NAME_LENGTH)
        {
            p_RecvPacket.rfinish();
            return;
        }

        l_ProfileData.KeepGroupsTogether            = p_RecvPacket.ReadBit();
        l_ProfileData.DisplayPets                   = p_RecvPacket.ReadBit();
        l_ProfileData.DisplayMainTankAndAssist      = p_RecvPacket.ReadBit();
        l_ProfileData.DisplayHealPrediction         = p_RecvPacket.ReadBit();
        l_ProfileData.DisplayAggroHighlight         = p_RecvPacket.ReadBit();
        l_ProfileData.DisplayOnlyDispellableDebuffs = p_RecvPacket.ReadBit();
        l_ProfileData.DisplayPowerBar               = p_RecvPacket.ReadBit();
        l_ProfileData.DisplayBorder                 = p_RecvPacket.ReadBit();
        l_ProfileData.UseClassColors                = p_RecvPacket.ReadBit();
        l_ProfileData.HorizontalGroups              = p_RecvPacket.ReadBit();
        l_ProfileData.DisplayNonBossDebuffs         = p_RecvPacket.ReadBit();
        l_ProfileData.DynamicPosition               = p_RecvPacket.ReadBit();
        l_ProfileData.Locked                        = p_RecvPacket.ReadBit();
        l_ProfileData.Shown                         = p_RecvPacket.ReadBit();
        l_ProfileData.AutoActivate2Players          = p_RecvPacket.ReadBit();
        l_ProfileData.AutoActivate3Players          = p_RecvPacket.ReadBit();
        l_ProfileData.AutoActivate5Players          = p_RecvPacket.ReadBit();
        l_ProfileData.AutoActivate10Players         = p_RecvPacket.ReadBit();
        l_ProfileData.AutoActivate15Players         = p_RecvPacket.ReadBit();
        l_ProfileData.AutoActivate25Players         = p_RecvPacket.ReadBit();
        l_ProfileData.AutoActivate40Players         = p_RecvPacket.ReadBit();
        l_ProfileData.AutoActivateSpec1             = p_RecvPacket.ReadBit();
        l_ProfileData.AutoActivateSpec2             = p_RecvPacket.ReadBit();
        l_ProfileData.AutoActivatePvP               = p_RecvPacket.ReadBit();
        l_ProfileData.AutoActivatePvE               = p_RecvPacket.ReadBit();

        p_RecvPacket >> l_ProfileData.FrameHeight;
        p_RecvPacket >> l_ProfileData.FrameWidth;
        p_RecvPacket >> l_ProfileData.SortBy;
        p_RecvPacket >> l_ProfileData.HealthText;
        p_RecvPacket >> l_ProfileData.TopPoint;
        p_RecvPacket >> l_ProfileData.BottomPoint;
        p_RecvPacket >> l_ProfileData.LeftPoint;
        p_RecvPacket >> l_ProfileData.TopOffset;
        p_RecvPacket >> l_ProfileData.BottomOffset;
        p_RecvPacket >> l_ProfileData.LeftOffset;

        l_Profile.Name = p_RecvPacket.ReadString(l_Profile.l_NameLen);
    }

    m_Player->SendCUFProfiles();

    SQLTransaction l_Transaction = CharacterDatabase.BeginTransaction();

    PreparedStatement* l_Stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CUF_PROFILE);
    l_Stmt->setUInt32(0, GetPlayer()->GetGUIDLow());
    l_Transaction->Append(l_Stmt);

    for (uint32 l_I = 0; l_I < l_ProfileCount; ++l_I)
    {
        CUFProfile& profile = l_Profiles[l_I];
        CUFProfileData data = profile.data;

        l_Stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_CUF_PROFILE);
        l_Stmt->setUInt32(0, GetPlayer()->GetGUIDLow());
        l_Stmt->setString(1, profile.Name);
        l_Stmt->setString(2, PackDBBinary(&data, sizeof(CUFProfileData)));
        l_Transaction->Append(l_Stmt);
    }

    CharacterDatabase.CommitTransaction(l_Transaction);
}
