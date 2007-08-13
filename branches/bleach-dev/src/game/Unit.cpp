/* 
 * Copyright (C) 2005,2006 MaNGOS <http://www.mangosproject.org/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "Log.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "World.h"
#include "ObjectMgr.h"
#include "Unit.h"
#include "QuestDef.h"
#include "Player.h"
#include "Creature.h"
#include "Spell.h"
#include "Stats.h"
#include "Group.h"
#include "SpellAuras.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "CreatureAI.h"
#include "Formulas.h"
#include "Pet.h"
#include "Util.h"

#include <math.h>

Unit::Unit() : Object()
{
    m_objectType |= TYPE_UNIT;
    m_objectTypeId = TYPEID_UNIT;

    m_attackTimer[BASE_ATTACK]   = 0;
    m_attackTimer[OFF_ATTACK]    = 0;
    m_attackTimer[RANGED_ATTACK] = 0;

    m_state = 0;
    m_form = 0;
    m_deathState = ALIVE;
    m_currentSpell = NULL;
    m_currentMeleeSpell = NULL;
    m_addDmgOnce = 0;
    m_TotemSlot1 = m_TotemSlot2 = m_TotemSlot3 = m_TotemSlot4  = 0;
    //m_Aura = NULL;
    //m_AurasCheck = 2000;
    //m_removeAuraTimer = 4;
    //tmpAura = NULL;
    m_silenced = false;
    waterbreath = false;

    m_detectStealth = 0;
    m_stealthvalue = 0;
    m_transform = 0;
    m_ShapeShiftForm = 0;

    for (int i = 0; i < TOTAL_AURAS; i++)
        m_AuraModifiers[i] = -1;
    for (int i = 0; i < IMMUNITY_MECHANIC; i++)
        m_spellImmune[i].clear();

    m_attacking = NULL;
    m_modDamagePCT = 0;
    m_RegenPCT = 0;
    m_modHitChance = 0;
    m_modSpellHitChance = 0;
    m_baseSpellCritChance = 5;
    m_modCastSpeedPct = 0;
    //m_spellCritSchool.clear();
    m_reflectSpellSchool.clear();
    m_scAuras.clear();
    /*m_damageDoneCreature.clear();
    m_damageDone.clear();
    m_damageTaken.clear();
    m_powerCostSchool.clear();
    m_creatureAttackPower.clear();*/
}

Unit::~Unit()
{
}

void Unit::Update( uint32 p_time )
{
    /*if(p_time > m_AurasCheck)
    {
        m_AurasCheck = 2000;
        _UpdateAura();
    }else
    m_AurasCheck -= p_time;*/

    _UpdateSpells( p_time );
    _UpdateHostil( p_time );

    if(uint32 base_att = getAttackTimer(BASE_ATTACK))
    {
        setAttackTimer(BASE_ATTACK, (p_time >= base_att ? 0 : base_att - p_time) );
    }
}

bool Unit::haveOffhandWeapon() const
{
    if(GetTypeId() == TYPEID_PLAYER)
    {
        Item *tmpitem = ((Player*)this)->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    
        return tmpitem && (tmpitem->GetProto()->InventoryType == INVTYPE_WEAPON || tmpitem->GetProto()->InventoryType == INVTYPE_WEAPONOFFHAND);
    }
    else
        return false;
}

void Unit::SendMoveToPacket(float x, float y, float z, bool run)
{
    float dx = x - GetPositionX();
    float dy = y - GetPositionY();
    float dz = z - GetPositionZ();
    float dist = ((dx*dx) + (dy*dy) + (dz*dz));
    if(dist<0)
        dist = 0;
    else
        dist = ::sqrt(dist);
    double speed = GetSpeed(run ? MOVE_RUN : MOVE_WALK);
    if(speed<=0)
        speed = 2.5f;
    speed *= 0.001f;
    uint32 time = static_cast<uint32>(dist / speed + 0.5);
    //float orientation = (float)atan2((double)dy, (double)dx);
    SendMonsterMove(x,y,z,false,run,time);

}

void Unit::SendMonsterMove(float NewPosX, float NewPosY, float NewPosZ, bool Walkback, bool Run, uint32 Time)
{
    WorldPacket data;
    data.Initialize( SMSG_MONSTER_MOVE );
    data << uint8(0xFF) << GetGUID();
                                                            // Point A, starting location
    data << GetPositionX() << GetPositionY() << GetPositionZ();
                                                            // little trick related to orientation
    data << (uint32)((*((uint32*)&GetOrientation())) & 0x30000000);
    data << uint8(Walkback);                                // walkback when walking from A to B
    data << uint32(Run ? 0x00000100 : 0x00000000);          // flags
    /* Flags:
    512: Floating, moving without walking/running
    */
    data << Time;                                           // Time in between points
    data << uint32(1);                                      // 1 single waypoint
    data << NewPosX << NewPosY << NewPosZ;                  // the single waypoint Point B
    WPAssert( data.size() == 50 );
    SendMessageToSet( &data, true );
}

void Unit::resetAttackTimer(WeaponAttackType type)
{
    if (GetTypeId() == TYPEID_PLAYER)
        m_attackTimer[type] = GetAttackTime(type);
    else
        m_attackTimer[type] = 2000;
}

bool Unit::canReachWithAttack(Unit *pVictim) const
{
    assert(pVictim);
    float reach = GetFloatValue(UNIT_FIELD_COMBATREACH);
    if( reach <= 0.0f )
        reach = 1.0f;
    float distance = GetDistanceSq(pVictim);

    return ( distance <= reach * reach );
}

void Unit::RemoveSpellsCausingAura(uint32 auraType)
{
    if (auraType >= TOTAL_AURAS) return;
    AuraList::iterator iter, next;
    for (iter = m_modAuras[auraType].begin(); iter != m_modAuras[auraType].end(); iter = next)
    {
        next = iter;
        ++next;

        if (*iter)
        {
            RemoveAurasDueToSpell((*iter)->GetId());
            if (!m_modAuras[auraType].empty())
                next = m_modAuras[auraType].begin();
            else
                return;
        }
    }
}

bool Unit::HasAuraType(uint32 auraType) const
{
    return (!m_modAuras[auraType].empty());
}

void Unit::DealDamage(Unit *pVictim, uint32 damage, uint32 procFlag, bool durabilityLoss)
{
    if (!pVictim->isAlive()) return;

    if(isStealth())
        RemoveSpellsCausingAura(SPELL_AURA_MOD_STEALTH);
    if(isInvisible())
        RemoveSpellsCausingAura(SPELL_AURA_MOD_INVISIBILITY);

    if(pVictim->GetTypeId() != TYPEID_PLAYER)
    {
        //pVictim->SetInFront(this);
        // no loot,xp,health if type 8 /critters/
        if ( ((Creature*)pVictim)->GetCreatureInfo()->type == 8)
        {
            pVictim->setDeathState(JUST_DIED);
            pVictim->SetHealth(0);
            pVictim->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_ATTACKING);
            return;
        }
        ((Creature*)pVictim)->AI().AttackStart(this);
    }

    DEBUG_LOG("DealDamageStart");

    uint32 health = pVictim->GetHealth();
    sLog.outDetail("deal dmg:%d to heals:%d ",damage,health);
    if (health <= damage)
    {
        DEBUG_LOG("DealDamage: victim just died");

        DEBUG_LOG("SET JUST_DIED");
        pVictim->setDeathState(JUST_DIED);

        uint64 attackerGuid, victimGuid;
        attackerGuid = GetGUID();
        victimGuid = pVictim->GetGUID();

        DEBUG_LOG("DealDamageAttackStop");
        SendAttackStop(victimGuid);
        pVictim->SendAttackStop(attackerGuid);

        DEBUG_LOG("DealDamageHealth1");
        pVictim->SetHealth(0);
        pVictim->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_ATTACKING);

        // 10% durability loss on death
        // clean hostilList
        if (pVictim->GetTypeId() == TYPEID_PLAYER)
        {
            DEBUG_LOG("We are dead, loosing 10 percents durability");
            if (durabilityLoss)
            {
                ((Player*)pVictim)->DeathDurabilityLoss(0.10);
            }
            HostilList::iterator i;
            for(i = m_hostilList.begin(); i != m_hostilList.end(); i++)
            {
                if(i->UnitGuid==victimGuid)
                {
                    m_hostilList.erase(i);
                    break;
                }
            }

            Creature *pet = pVictim->GetPet();
            if(pet && pet->isPet())
            {
                pet->setDeathState(JUST_DIED);
                pet->SendAttackStop(attackerGuid);
                pet->SetHealth(0);
                pet->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_ATTACKING);
                pet->addUnitState(UNIT_STAT_DIED);
                for(i = m_hostilList.begin(); i != m_hostilList.end(); ++i)
                {
                    if(i->UnitGuid==pet->GetGUID())
                    {
                        m_hostilList.erase(i);
                        break;
                    }
                }
            }
        }
        else
        {
            pVictim->m_hostilList.clear();
            DEBUG_LOG("DealDamageNotPlayer");
            pVictim->SetUInt32Value(UNIT_DYNAMIC_FLAGS, 1);
        }

        //judge if GainXP, Pet kill like player kill,kill pet not like PvP
        bool PvP = false;
        Player *player = 0;

        if(GetTypeId() == TYPEID_PLAYER)
        {
            player = (Player*)this;
            if(pVictim->GetTypeId() == TYPEID_PLAYER)
                PvP = true;
        }
        else if(((Creature*)this)->isPet())
        {
            Unit* owner = ((Pet*)this)->GetOwner();

            if(owner && owner->GetTypeId() == TYPEID_PLAYER)
            {
                player = (Player*)owner;
            }
        }

        // self or owner of pet
        if(player) 
        {
            player->CalculateHonor(pVictim);
            player->CalculateReputation(pVictim);

            if(!PvP)
            {
                DEBUG_LOG("DealDamageIsPvE");
                uint32 xp = MaNGOS::XP::Gain(player, pVictim);
                uint32 entry = 0;
                entry = pVictim->GetUInt32Value(OBJECT_FIELD_ENTRY );

                Group *pGroup = objmgr.GetGroupByLeader(player->GetGroupLeader());
                if(pGroup)
                {
                    DEBUG_LOG("Kill Enemy In Group");
                    xp /= pGroup->GetMembersCount();
                    for (uint32 i = 0; i < pGroup->GetMembersCount(); i++)
                    {
                        Player *pGroupGuy = ObjectAccessor::Instance().FindPlayer(pGroup->GetMemberGUID(i));
                        if(!pGroupGuy)
                            continue;
                        if(GetDistanceSq(pGroupGuy) > sWorld.getConfig(CONFIG_GETXP_DISTANCE))
                            continue;
                        if(uint32(abs((int)pGroupGuy->getLevel() - (int)pVictim->getLevel())) > sWorld.getConfig(CONFIG_GETXP_LEVELDIFF))
                            continue;
                        pGroupGuy->GiveXP(xp, pVictim);
                        pGroupGuy->KilledMonster(entry, victimGuid);
                    }
                }
                else
                {
                    DEBUG_LOG("Player kill enemy alone");
                    player->GiveXP(xp, pVictim);
                    player->KilledMonster(entry,victimGuid);
                }
            }
        }
        else
        {
            DEBUG_LOG("Monster kill Monster");
            SendAttackStop(victimGuid);
            pVictim->addUnitState(UNIT_STAT_DIED);
        }
        AttackStop();
    }
    else
    {
        DEBUG_LOG("DealDamageAlive");
        pVictim->SetHealth(health - damage);
        Attack(pVictim);

        if(pVictim->getTransForm())
        {
            pVictim->RemoveAurasDueToSpell(pVictim->getTransForm());
            pVictim->setTransForm(0);
        }

        if (pVictim->GetTypeId() != TYPEID_PLAYER)
        {
            ((Creature *)pVictim)->AI().DamageInflict(this, damage);
            pVictim->AddHostil(GetGUID(), damage);
            if( GetTypeId() == TYPEID_PLAYER
                && (getClass() == WARRIOR || m_form == 5 || m_form == 8)
                && !m_currentMeleeSpell)                    // not generate rage for special attacks
                ((Player*)this)->CalcRage(damage,true);
        }
        else
        {
            if( pVictim->getClass() == WARRIOR )
                ((Player*)pVictim)->CalcRage(damage,false);

            // random durability for items (HIT)
            int randdurability = urand(0, 300);
            if (randdurability == 10)
            {
                DEBUG_LOG("HIT: We decrease durability with 5 percent");
                ((Player*)pVictim)->DeathDurabilityLoss(0.05);
            }
        }
    }

    DEBUG_LOG("DealDamageEnd");
}

void Unit::CastSpell(Unit* Victim, uint32 spellId, bool triggered)
{

    SpellEntry *spellInfo = sSpellStore.LookupEntry(spellId );

    if(!spellInfo)
    {
        sLog.outError("WORLD: unknown spell id %i\n", spellId);
        return;
    }

    Spell *spell = new Spell(this, spellInfo, triggered, 0);
    WPAssert(spell);

    SpellCastTargets targets;
    targets.setUnitTarget( Victim );
    spell->prepare(&targets);
    m_canMove = false;
    if (triggered) delete spell;
}

void Unit::SpellNonMeleeDamageLog(Unit *pVictim, uint32 spellID, uint32 damage)
{
    if(!this || !pVictim)
        return;
    if(!this->isAlive() || !pVictim->isAlive())
        return;
    SpellEntry *spellInfo = sSpellStore.LookupEntry(spellID);
    if(!spellInfo)
        return;
    uint32 absorb=0;
    uint32 resist=0;

    uint32 pdamage = SpellDamageBonus(pVictim,spellInfo,damage);
    absorb = CalDamageAbsorb(pVictim,spellInfo->School,pdamage,&resist);

    //WorldPacket data;
    if(m_modSpellHitChance+100 < urand(0,100))
    {
        SendAttackStateUpdate(HITINFO_MISS, pVictim->GetGUID(), 1, spellInfo->School, 0, 0,0,1,0);
        return;
    }

    // Only send absorbed message if we actually absorbed some damage
    if( damage <= absorb+resist && absorb)
    {
        SendAttackStateUpdate(HITINFO_ABSORB|HITINFO_SWINGNOHITSOUND, pVictim->GetGUID(), 1, spellInfo->School, pdamage, absorb,resist,1,0);
        return;
    }

    sLog.outDetail("SpellNonMeleeDamageLog: %u %X attacked %u %X for %u dmg inflicted by %u,abs is %u,resist is %u",
        GetGUIDLow(), GetGUIDHigh(), pVictim->GetGUIDLow(), pVictim->GetGUIDHigh(), pdamage, spellID, absorb, resist);

    SendSpellNonMeleeDamageLog(pVictim->GetGUID(), spellID, pdamage, spellInfo->School, absorb, resist, false, 0);
    DealDamage(pVictim, pdamage<(absorb+resist)?0:(pdamage-absorb-resist), 0, true);
}

void Unit::PeriodicAuraLog(Unit *pVictim, SpellEntry *spellProto, Modifier *mod)
{
    uint32 procFlag = 0;
    if(!this || !pVictim || !isAlive() || !pVictim->isAlive())
    {
        return;
    }
    uint32 absorb=0;
    uint32 resist=0;

    uint32 pdamage = SpellDamageBonus(pVictim,spellProto,mod->m_amount);
    absorb = CalDamageAbsorb(pVictim,spellProto->School,pdamage,&resist);

    sLog.outDetail("PeriodicAuraLog: %u %X attacked %u %X for %u dmg inflicted by %u abs is %u",
        GetGUIDLow(), GetGUIDHigh(), pVictim->GetGUIDLow(), pVictim->GetGUIDHigh(), pdamage, spellProto->Id,absorb);

    WorldPacket data;
    data.Initialize(SMSG_PERIODICAURALOG);
    data << uint8(0xFF) << pVictim->GetGUID();
    data << uint8(0xFF) << this->GetGUID();
    data << spellProto->Id;
    data << uint32(1);

    data << mod->m_auraname;
    data << (uint32)(mod->m_amount);
    data << spellProto->School;
    data << uint32(0);
    SendMessageToSet(&data,true);

    if(mod->m_auraname == SPELL_AURA_PERIODIC_DAMAGE)
    {
        SendSpellNonMeleeDamageLog(pVictim->GetGUID(), spellProto->Id, mod->m_amount, spellProto->School, absorb, resist, false, 0);
        SendMessageToSet(&data,true);

        DealDamage(pVictim, mod->m_amount <= int32(absorb+resist) ? 0 : (mod->m_amount-absorb-resist), procFlag, true);
    }
    else if(mod->m_auraname == SPELL_AURA_PERIODIC_DAMAGE_PERCENT)
    {
        int32 pdamage = GetHealth()*(100+mod->m_amount)/100;
        SendSpellNonMeleeDamageLog(pVictim->GetGUID(), spellProto->Id, pdamage, spellProto->School, absorb, resist, false, 0);
        SendMessageToSet(&data,true);
        DealDamage(pVictim, pdamage <= int32(absorb+resist) ? 0 : (pdamage-absorb-resist), procFlag, true);
    }
    else if(mod->m_auraname == SPELL_AURA_PERIODIC_HEAL)
    {
        int32 pdamage = mod->m_amount*(100+m_RegenPCT)/100;
        if(GetHealth() + pdamage < GetMaxHealth() )
            SetHealth(GetHealth() + pdamage);
        else
            SetHealth(GetMaxHealth());
        if(pVictim->GetTypeId() == TYPEID_PLAYER || GetTypeId() == TYPEID_PLAYER)
            SendHealSpellOnPlayer(pVictim, spellProto->Id, pdamage);
    }
    else if(mod->m_auraname == SPELL_AURA_PERIODIC_LEECH)
    {
        uint32 tmpvalue = 0;
        float tmpvalue2 = 0;
        for(int x=0;x<3;x++)
        {
            if(mod->m_auraname != spellProto->EffectApplyAuraName[x])
                continue;
            tmpvalue2 = spellProto->EffectMultipleValue[x];
            tmpvalue2 = tmpvalue2 > 0 ? tmpvalue2 : 1;

            if(pVictim->GetHealth() - mod->m_amount > 0)
                tmpvalue = uint32(mod->m_amount*tmpvalue2);
            else
                tmpvalue = uint32(pVictim->GetHealth()*tmpvalue2);

            SendSpellNonMeleeDamageLog(pVictim->GetGUID(), spellProto->Id, tmpvalue, spellProto->School, absorb, resist, false, 0);
            DealDamage(pVictim, mod->m_amount <= int32(absorb+resist) ? 0 : (mod->m_amount-absorb-resist), procFlag, false);
            if (!pVictim->isAlive() && m_currentSpell)
                if (m_currentSpell->m_spellInfo)
                    if (m_currentSpell->m_spellInfo->Id == spellProto->Id)
                        m_currentSpell->cancel();

            break;
        }
        tmpvalue2 = (100+m_RegenPCT)/100;
        tmpvalue = uint32(tmpvalue*tmpvalue2);
        if(GetHealth() + tmpvalue < GetMaxHealth() )
            SetHealth(GetHealth() + tmpvalue);
        else SetHealth(GetMaxHealth());
        if(pVictim->GetTypeId() == TYPEID_PLAYER || GetTypeId() == TYPEID_PLAYER)
            pVictim->SendHealSpellOnPlayer(this, spellProto->Id, tmpvalue);
    }
    else if(mod->m_auraname == SPELL_AURA_PERIODIC_MANA_LEECH)
    {
        uint32 tmpvalue = 0;
        for(int x=0;x<3;x++)
        {
            if(mod->m_auraname != spellProto->EffectApplyAuraName[x])
                continue;
            if(pVictim->GetPower(POWER_MANA) - mod->m_amount > 0)
            {
                pVictim->SetPower(POWER_MANA,pVictim->GetPower(POWER_MANA) - mod->m_amount);
                tmpvalue = uint32(mod->m_amount*spellProto->EffectMultipleValue[x]);
            }
            else
            {
                tmpvalue = uint32(pVictim->GetPower(POWER_MANA)*spellProto->EffectMultipleValue[x]);
                pVictim->SetPower(POWER_MANA,0);
            }
            break;
        }
        if(GetPower(POWER_MANA) + tmpvalue < GetMaxPower(POWER_MANA) )
            SetPower(POWER_MANA,GetPower(POWER_MANA) + tmpvalue);
        else SetPower(POWER_MANA,GetMaxPower(POWER_MANA));
    }
    else if(mod->m_auraname == SPELL_AURA_PERIODIC_ENERGIZE)
    {
        if(mod->m_miscvalue < 0 || mod->m_miscvalue > 4)
            return;
        SetPower(Powers(mod->m_miscvalue),GetPower(Powers(mod->m_miscvalue))+mod->m_amount);
    }
}

void Unit::HandleEmoteCommand(uint32 anim_id)
{
    WorldPacket data;

    data.Initialize( SMSG_EMOTE );
    data << anim_id << GetGUID();
    WPAssert(data.size() == 12);

    SendMessageToSet(&data, true);
}

uint32 Unit::CalDamageAbsorb(Unit *pVictim,uint32 School,const uint32 damage,uint32 *resist)
{
    uint32 AbsorbDamage=0;
    uint32 currAbsorbDamage=0;
    uint32 currentPower;
    bool  removeAura=false;

    if(!pVictim)
        return 0;
    if(!pVictim->isAlive())
        return 0;
    if(!damage)
        return 0;
    for(std::list<struct DamageManaShield*>::iterator i = pVictim->m_damageManaShield.begin();i != pVictim->m_damageManaShield.end();i++)
    {
        SpellEntry *spellInfo = sSpellStore.LookupEntry( (*i)->m_spellId);

        if(((*i)->m_schoolType & School) || (*i)->m_schoolType == School || (*i)->m_schoolType ==127)
        {
            currAbsorbDamage = damage+ (*i)->m_currAbsorb;
            if(currAbsorbDamage < (*i)->m_totalAbsorb)
            {
                AbsorbDamage = damage;
                (*i)->m_currAbsorb = currAbsorbDamage;
            }
            else
            {
                AbsorbDamage = (*i)->m_totalAbsorb - (*i)->m_currAbsorb;
                (*i)->m_currAbsorb = (*i)->m_totalAbsorb;
                removeAura = true;
            }

            if((*i)->m_modType == SPELL_AURA_MANA_SHIELD)
            {
                float multiple;
                for(int x=0;x<3;x++)
                    if(spellInfo->EffectApplyAuraName[x] == SPELL_AURA_MANA_SHIELD)
                {
                    multiple = spellInfo->EffectMultipleValue[x];
                    break;
                }
                currentPower = pVictim->GetPower(POWER_MANA);
                if ( (float)(currentPower) > AbsorbDamage*multiple )
                {
                    pVictim->SetPower(POWER_MANA, (uint32)(currentPower-AbsorbDamage*multiple) );
                }
                else
                {
                    pVictim->SetPower(POWER_MANA, 0 );
                }
            }

            if(removeAura)
                pVictim->RemoveAurasDueToSpell((*i)->m_spellId);
        }
        break;
    }
    if(School == 0)
    {
        uint32 armor = pVictim->GetArmor();
        float tmpvalue = armor/(pVictim->getLevel()*85.0 +400.0 +armor);
        if(tmpvalue < 0)
            tmpvalue = 0.0;
        if(tmpvalue > 1.0)
            tmpvalue = 1.0;
        AbsorbDamage += uint32(damage * tmpvalue);
        if(AbsorbDamage > damage)
            AbsorbDamage = damage;
    }
    if( School > 0)
    {
        uint32 tmpvalue2 = pVictim->GetResistance(SpellSchools(School));
        *resist += uint32(damage*tmpvalue2*0.0025*pVictim->getLevel()/getLevel());
        if(*resist > damage)
            *resist = damage;
    }

    // random durability loss for items on absorb (ABSORB)
    if (pVictim->GetTypeId() == TYPEID_PLAYER)
    {
        int randdurability = urand(0, 300);
        if (randdurability == 10)
        {
            DEBUG_LOG("BLOCK: We decrease durability with 5 percent");
            ((Player*)pVictim)->DeathDurabilityLoss(0.05);
        }
    }

    return AbsorbDamage;
}

void Unit::DoAttackDamage (Unit *pVictim, uint32 *damage, uint32 *blocked_amount, uint32 *damageType, uint32 *hitInfo, uint32 *victimState, uint32 *absorbDamage, uint32 *resist, WeaponAttackType attType)
{
    // this unit's proc trigger damage and spell
    for (AuraMap::iterator i = m_Auras.begin(); i != m_Auras.end(); ++i)
    {
        ProcTriggerDamage *procdamage = (*i).second->GetProcDamage();
        if(procdamage)
        {
            bool nocharges = (*i).second->GetSpellProto()->procCharges == 0 ? true : false;
            if( (procdamage->procFlags == 0) && procdamage->procChance > rand_chance()
                && (procdamage->procCharges > 0 || nocharges))
            {
                SpellNonMeleeDamageLog(pVictim,(*i).second->GetSpellProto()->Id,procdamage->procDamage);
                if(!nocharges)
                {
                    procdamage->procCharges -= 1;
                    if(procdamage->procCharges == 0)
                        (*i).second->RemoveProcDamage();
                }
            }
        }
        if(ProcTriggerSpell* procspell = (*i).second->GetProcSpell())
        {
            bool nocharges = (*i).second->GetSpellProto()->procCharges == 0 ? true : false;
            if((procspell->procFlags == 0) && procspell->procChance > rand_chance()
                && (procspell->procCharges > 0 || nocharges))
            {
                SpellEntry *spellInfo = sSpellStore.LookupEntry((*i).second->GetProcSpell()->trigger );

                if(!spellInfo)
                {
                    sLog.outError("WORLD: unknown spell id %i\n", (*i).second->GetProcSpell()->trigger);
                    return;
                }

                Spell spell(this, spellInfo, true, 0);

                SpellCastTargets targets;
                targets.setUnitTarget( pVictim );
                spell.prepare(&targets);
                if(!nocharges)
                {
                    procspell->procCharges -= 1;
                    if(procspell->procCharges == 0)
                        (*i).second->RemoveProcSpell();
                }
            }
        }
    }
    MeleeHitOutcome outcome = RollMeleeOutcomeAgainst (pVictim, attType);
    if (outcome == MELEE_HIT_MISS)
    {
        *hitInfo |= HITINFO_MISS;
        return;
    }

    CreatureInfo *cinfo = NULL;
    if(pVictim->GetTypeId() != TYPEID_PLAYER)
        cinfo = ((Creature*)pVictim)->GetCreatureInfo();

    *damage = MeleeDamageBonus(pVictim, CalculateDamage (attType));

    if(GetTypeId() == TYPEID_PLAYER && pVictim->GetTypeId() != TYPEID_PLAYER && ((Creature*)pVictim)->GetCreatureInfo()->type != 8 )
        ((Player*)this)->UpdateMeleeSkillWeapon (attType);

    switch (outcome)
    {
        case MELEE_HIT_CRIT:
            //*hitInfo = 0xEA;
            *hitInfo  = HITINFO_CRITICALHIT | HITINFO_NORMALSWING2 | 0x8;               // 0xEA
            *damage *= 2;

            pVictim->HandleEmoteCommand(EMOTE_ONESHOT_WOUNDCRITICAL);
            break;

        case MELEE_HIT_PARRY:
            *damage = 0;
            *victimState = 3;

            // instant (maybe with small delay) counter attack
            {
                uint32 offtime  = pVictim->getAttackTimer(OFF_ATTACK);
                uint32 basetime = pVictim->getAttackTimer(BASE_ATTACK);

                if (pVictim->haveOffhandWeapon() && offtime < basetime)
                {
                    if( offtime > ATTACK_DISPLAY_DELAY )
                        pVictim->setAttackTimer(OFF_ATTACK,ATTACK_DISPLAY_DELAY);
                }
                else
                {
                    if ( basetime > ATTACK_DISPLAY_DELAY )
                        pVictim->setAttackTimer(BASE_ATTACK,ATTACK_DISPLAY_DELAY);
                }
            }

            if(pVictim->GetTypeId() == TYPEID_PLAYER)
                ((Player*)pVictim)->UpdateDefense();

            pVictim->HandleEmoteCommand(EMOTE_ONESHOT_PARRYUNARMED);
            break;

        case MELEE_HIT_DODGE:
            *damage = 0;
            *victimState = 2;

            if(pVictim->GetTypeId() == TYPEID_PLAYER)
                ((Player*)pVictim)->UpdateDefense();

            pVictim->HandleEmoteCommand(EMOTE_ONESHOT_PARRYUNARMED);
            break;

        case MELEE_HIT_BLOCK:
            *blocked_amount = (pVictim->GetBlockValue() + (pVictim->GetStat(STAT_STRENGTH) / 20) -1);

            if (pVictim->GetUnitBlockChance())
                pVictim->HandleEmoteCommand(EMOTE_ONESHOT_PARRYSHIELD);
            else
                pVictim->HandleEmoteCommand(EMOTE_ONESHOT_PARRYUNARMED);

            *victimState = 5;

            if(pVictim->GetTypeId() == TYPEID_PLAYER)
                ((Player*)pVictim)->UpdateDefense();
            break;

        case MELEE_HIT_GLANCING:
        {
            // 30% reduction at 15 skill diff, no reduction at 5 skill diff
            int32 reducePerc = 100 - (pVictim->GetDefenceSkillValue() - GetWeaponSkillValue(attType) - 5) * 3;
            if (reducePerc < 70)
                reducePerc = 70;
            *damage = *damage * reducePerc / 100;
            *hitInfo |= HITINFO_GLANCING;
            break;
        }
        case MELEE_HIT_CRUSHING:
            // 150% normal damage
            *damage += (*damage / 2);
            *hitInfo |= HITINFO_CRUSHING;
            // TODO: victimState, victim animation?
            break;

        default:
            break;
    }

    for(std::list<struct DamageShield>::iterator i = pVictim->m_damageShields.begin();i != pVictim->m_damageShields.end();i++)
    {
        pVictim->SpellNonMeleeDamageLog(this,i->m_spellId,i->m_damage);
    }
    uint32 absorb= CalDamageAbsorb(pVictim,NORMAL_DAMAGE,*damage,resist);

    if (*damage <= absorb + *resist + *blocked_amount)
    {
        //*hitInfo = 0x00010020;
        *hitInfo = HITINFO_NOACTION | HITINFO_ABSORB;
        *absorbDamage = absorb;
        *damageType = 0;
        return;
    }
    else
    {
        *absorbDamage = absorb;
    }
    // Fix me : when the procdamage->procCharges == 0 and nocharges == false,remove this aura.
    // pVictim's proc trigger damage and spell
    for (AuraMap::iterator i = pVictim->m_Auras.begin(); i != pVictim->m_Auras.end(); ++i)
    {
        ProcTriggerDamage *procdamage = (*i).second->GetProcDamage();
        if(procdamage)
        {
            bool nocharges = (*i).second->GetSpellProto()->procCharges == 0 ? true : false;
            if((procdamage->procFlags & 40) && procdamage->procChance > rand_chance()
                && (procdamage->procCharges > 0 || nocharges))
            {
                pVictim->SpellNonMeleeDamageLog(this,(*i).second->GetSpellProto()->Id,procdamage->procDamage);
                if(!nocharges)
                {
                    procdamage->procCharges -= 1;
                    if(procdamage->procCharges == 0)
                        (*i).second->RemoveProcDamage();
                }
            }
        }
        if(ProcTriggerSpell* procspell = (*i).second->GetProcSpell())
        {
            bool nocharges = (*i).second->GetSpellProto()->procCharges == 0 ? true : false;
            if((procspell->procFlags & 40)  && procspell->procChance > rand_chance()
                && (procspell->procCharges > 0 || nocharges))
            {
                SpellEntry *spellInfo = sSpellStore.LookupEntry((*i).second->GetProcSpell()->trigger);

                if(!spellInfo)
                {
                    sLog.outError("WORLD: unknown spell id %i\n", (*i).second->GetProcSpell()->trigger);
                    return;
                }

                Spell spell(pVictim, spellInfo, true, 0);

                SpellCastTargets targets;
                targets.setUnitTarget( this );
                spell.prepare(&targets);

                if((*i).second->GetProcSpell()->trigger == 26545)
                    pVictim->SpellNonMeleeDamageLog(this,(*i).second->GetSpellProto()->Id,(*i).second->CalculateDamage());
                if(!nocharges)
                {
                    procspell->procCharges -= 1;
                    if(procspell->procCharges == 0)
                        (*i).second->RemoveProcSpell();
                }
            }
        }
    }
    // this unit's proc trigger damage and spell
    for (AuraMap::iterator i = m_Auras.begin(); i != m_Auras.end(); ++i)
    {
        ProcTriggerDamage *procdamage = (*i).second->GetProcDamage();
        if(procdamage)
        {
            bool nocharges = (*i).second->GetSpellProto()->procCharges == 0 ? true : false;
            if( (procdamage->procFlags & 20) && procdamage->procChance > rand_chance()
                && (procdamage->procCharges > 0 || nocharges))
            {
                SpellNonMeleeDamageLog(pVictim,(*i).second->GetSpellProto()->Id,procdamage->procDamage);
                if(!nocharges)
                {
                    procdamage->procCharges -= 1;
                    if(procdamage->procCharges == 0)
                        (*i).second->RemoveProcDamage();
                }
            }
        }
        if(ProcTriggerSpell* procspell = (*i).second->GetProcSpell())
        {
            bool nocharges = (*i).second->GetSpellProto()->procCharges == 0 ? true : false;
            if((procspell->procFlags & 20) && procspell->procChance > rand_chance()
                && (procspell->procCharges > 0 || nocharges))
            {
                SpellEntry *spellInfo = sSpellStore.LookupEntry((*i).second->GetProcSpell()->trigger );

                if(!spellInfo)
                {
                    sLog.outError("WORLD: unknown spell id %i\n", (*i).second->GetProcSpell()->trigger);
                    return;
                }

                Spell spell(this, spellInfo, true, 0);

                SpellCastTargets targets;
                targets.setUnitTarget( pVictim );
                spell.prepare(&targets);
                if(!nocharges)
                {
                    procspell->procCharges -= 1;
                    if(procspell->procCharges == 0)
                        (*i).second->RemoveProcSpell();
                }
            }
        }
    }

    if(pVictim->m_currentSpell && pVictim->GetTypeId() == TYPEID_PLAYER && *damage)
    {
        if (pVictim->m_currentSpell->getState() != SPELL_STATE_CASTING)
        {
            sLog.outString("Spell Delayed!%d",(int32)(0.25f * pVictim->m_currentSpell->casttime));
            pVictim->m_currentSpell->Delayed((int32)(0.25f * pVictim->m_currentSpell->casttime));
        }
        else
        {
            sLog.outString("Spell Canceled!");
            pVictim->m_currentSpell->cancel();



        }
    }

}

void Unit::AttackerStateUpdate (Unit *pVictim, WeaponAttackType attType)
{

    if(hasUnitState(UNIT_STAT_CONFUSED) || hasUnitState(UNIT_STAT_STUNDED))
        return;

    if (!pVictim->isAlive())
    {
        SendAttackStop(pVictim->GetGUID());
        return;
    }

    if(m_currentSpell)
        return;

    if (m_currentMeleeSpell)
    {
        m_currentMeleeSpell->cast();
        return;
    }

    uint32 hitInfo;
    if (attType == BASE_ATTACK)
        hitInfo = HITINFO_NORMALSWING2;
    else if (attType == OFF_ATTACK)
        hitInfo = HITINFO_LEFTSWING;
    else
        return;

    uint32   damageType = NORMAL_DAMAGE;
    uint32   victimState = VICTIMSTATE_NORMAL;

    uint32   damage = 0;
    uint32   blocked_dmg = 0;
    uint32   absorbed_dmg = 0;
    uint32   resisted_dmg = 0;

    DoAttackDamage (pVictim, &damage, &blocked_dmg, &damageType, &hitInfo, &victimState, &absorbed_dmg, &resisted_dmg, attType);

    if (hitInfo & HITINFO_MISS)
        //send miss
        SendAttackStateUpdate (hitInfo, pVictim->GetGUID(), 1, damageType, damage, absorbed_dmg, resisted_dmg, victimState, blocked_dmg);
    else
    {
        if (absorbed_dmg)hitInfo |= HITINFO_ABSORB;
        if (resisted_dmg)hitInfo |= HITINFO_RESIST;
        if ((absorbed_dmg || resisted_dmg) && ((absorbed_dmg + resisted_dmg + blocked_dmg) > damage)) hitInfo |= HITINFO_SWINGNOHITSOUND;

        //do animation
        SendAttackStateUpdate (hitInfo, pVictim->GetGUID(), 1, damageType, damage, absorbed_dmg, resisted_dmg, victimState, blocked_dmg);

        if (damage > (absorbed_dmg + resisted_dmg + blocked_dmg))
            damage -= (absorbed_dmg + resisted_dmg + blocked_dmg);
        else
            damage = 0;
        DealDamage (pVictim, damage, 0, true);

        if(GetTypeId() == TYPEID_PLAYER && pVictim->isAlive())
        {
            for(int i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; i++)
                ((Player*)this)->CastItemCombatSpell(((Player*)this)->GetItemByPos(INVENTORY_SLOT_BAG_0,i),pVictim);
        }
    }

    if (GetTypeId() == TYPEID_PLAYER)
        DEBUG_LOG("AttackerStateUpdate: (Player) %u %X attacked %u %X for %u dmg, absorbed %u, blocked %u, resisted %u.",
            GetGUIDLow(), GetGUIDHigh(), pVictim->GetGUIDLow(), pVictim->GetGUIDHigh(), damage, absorbed_dmg, blocked_dmg, resisted_dmg);
    else
        DEBUG_LOG("AttackerStateUpdate: (NPC)    %u %X attacked %u %X for %u dmg, absorbed %u, blocked %u, resisted %u.",
            GetGUIDLow(), GetGUIDHigh(), pVictim->GetGUIDLow(), pVictim->GetGUIDHigh(), damage, absorbed_dmg, blocked_dmg, resisted_dmg);
}

MeleeHitOutcome Unit::RollMeleeOutcomeAgainst (const Unit *pVictim, WeaponAttackType attType) const
{
    int32 skillDiff =  GetWeaponSkillValue(attType) - pVictim->GetDefenceSkillValue();
    // bonus from skills is 0.04%
    int32    skillBonus = skillDiff * 4;
    int32    sum = 0, tmp = 0;
    int32    roll = urand (0, 10000);

    DEBUG_LOG ("RollMeleeOutcomeAgainst: skill bonus of %d for attacker", skillBonus);
    DEBUG_LOG ("RollMeleeOutcomeAgainst: rolled %d, +hit %d, dodge %u, parry %u, block %u, crit %u",
        roll, m_modHitChance, (uint32)(pVictim->GetUnitDodgeChance()*100), (uint32)(pVictim->GetUnitParryChance()*100),
        (uint32)(pVictim->GetUnitBlockChance()*100), (uint32)(GetUnitCriticalChance()*100));

    // dual wield has 24% base chance to miss instead of 5%, also
    // base miss rate is 5% and can't get higher than 60%
    if(haveOffhandWeapon())
    {
        tmp = 2400 - skillBonus - m_modHitChance*100;
    }
    else
        tmp = 500 - skillBonus - m_modHitChance*100;

    if(tmp > 6000)
        tmp = 6000;

    if (tmp > 0 && roll < (sum += tmp ))
    {
        DEBUG_LOG ("RollMeleeOutcomeAgainst: MISS");
        return MELEE_HIT_MISS;
    }

    // always crit against a sitting target
    if (   (pVictim->GetTypeId() == TYPEID_PLAYER)
        && (((Player*)pVictim)->getStandState() & (PLAYER_STATE_SLEEP | PLAYER_STATE_SIT
        | PLAYER_STATE_SIT_CHAIR
        | PLAYER_STATE_SIT_LOW_CHAIR
        | PLAYER_STATE_SIT_MEDIUM_CHAIR
        | PLAYER_STATE_SIT_HIGH_CHAIR)))
    {
        DEBUG_LOG ("RollMeleeOutcomeAgainst: CRIT (sitting victim)");
        return MELEE_HIT_CRIT;
    }

    // stunned target cannot dodge and this is check in GetUnitDodgeChance()
    tmp = (int32)(pVictim->GetUnitDodgeChance()*100) - skillBonus;
    if (tmp > 0 && roll < (sum += tmp))
    {
        DEBUG_LOG ("RollMeleeOutcomeAgainst: DODGE <%d, %d)", sum-tmp, sum);
        return MELEE_HIT_DODGE;
    }

    int32   modCrit = 0;

    // check if attack comes from behind
    if (!pVictim->HasInArc(M_PI,this))
    {
        // ASSUME +10% crit from behind
        DEBUG_LOG ("RollMeleeOutcomeAgainst: attack came from behind.");
        modCrit += 1000;
    }
    else
    {
        // cannot parry or block attacks from behind, but can from forward
        tmp = (int32)(pVictim->GetUnitParryChance()*100);
        if (   (tmp > 0)                                    // check if unit _can_ parry
            && ((tmp -= skillBonus) > 0)
            && (roll < (sum += tmp)))
        {
            DEBUG_LOG ("RollMeleeOutcomeAgainst: PARRY <%d, %d)", sum-tmp, sum);
            return MELEE_HIT_PARRY;
        }

        tmp = (int32)(pVictim->GetUnitBlockChance()*100);
        if (   (tmp > 0)                                    // check if unit _can_ block
            && ((tmp -= skillBonus) > 0)
            && (roll < (sum += tmp)))
        {
            DEBUG_LOG ("RollMeleeOutcomeAgainst: BLOCK <%d, %d)", sum-tmp, sum);
            return MELEE_HIT_BLOCK;
        }
    }

    // flat 40% chance to score a glancing blow if you're 3 or more levels
    // below mob level or your weapon skill is too low
    if (   (GetTypeId() == TYPEID_PLAYER)
        && (pVictim->GetTypeId() != TYPEID_PLAYER)
        && ((getLevel() + 3 <= pVictim->getLevel()) || (skillDiff <= -15))
        && (roll < (sum += 4000)))
    {
        DEBUG_LOG ("RollMeleeOutcomeAgainst: GLANCING <%d, %d)", sum-4000, sum);
        return MELEE_HIT_GLANCING;
    }

    // FIXME: +skill and +defense has no effect on crit chance in PvP combat
    tmp = (int32)(GetUnitCriticalChance()*100) + skillBonus + modCrit;
    if (tmp > 0 && roll < (sum += tmp))
    {
        DEBUG_LOG ("RollMeleeOutcomeAgainst: CRIT <%d, %d)", sum-tmp, sum);
        return MELEE_HIT_CRIT;
    }

    // mobs can score crushing blows if they're 3 or more levels above victim
    // or when their weapon skill is 15 or more above victim's defense skill
    if ( (GetTypeId() != TYPEID_PLAYER)
        && ((getLevel() >= pVictim->getLevel() + 3) || (skillDiff >= 15)))
    {
        // tmp = player's max defense skill - player's current defense skill
        tmp = 5*pVictim->getLevel() - pVictim->GetDefenceSkillValue();
        // having defense above your maximum (from items, talents etc.) has no effect
        // add 2% chance per lacking skill point, min. is 15%
        // FIXME: chance should go up with mob lvl
        tmp = 1500 + (tmp > 0 ? tmp*200 : 0);
        if (roll < (sum += tmp))
        {
            DEBUG_LOG ("RollMeleeOutcomeAgainst: CRUSHING <%d, %d)", sum-tmp, sum);
            return MELEE_HIT_CRUSHING;
        }
    }

    DEBUG_LOG ("RollMeleeOutcomeAgainst: NORMAL");
    return MELEE_HIT_NORMAL;
}

uint32 Unit::CalculateDamage (WeaponAttackType attType)
{
    float min_damage, max_damage;

    switch (attType)
    {
        case RANGED_ATTACK:
            min_damage = GetFloatValue(UNIT_FIELD_MINRANGEDDAMAGE);
            max_damage = GetFloatValue(UNIT_FIELD_MAXRANGEDDAMAGE);
            break;
        case BASE_ATTACK:
            min_damage = GetFloatValue(UNIT_FIELD_MINDAMAGE);
            max_damage = GetFloatValue(UNIT_FIELD_MAXDAMAGE);
            break;
        case OFF_ATTACK:
            min_damage = GetFloatValue(UNIT_FIELD_MINOFFHANDDAMAGE) * 0.5; // TODO: add offhand dmg from talents
            max_damage = GetFloatValue(UNIT_FIELD_MAXOFFHANDDAMAGE) * 0.5;
            break;
    }

    if (min_damage > max_damage)
    {
        std::swap(min_damage,max_damage);
    }

    if(max_damage == 0.0)
        max_damage = 5.0;

    return urand ((uint32)min_damage, (uint32)max_damage);
}

void Unit::SendAttackStop(uint64 victimGuid)
{
    WorldPacket data;
    data.Initialize( SMSG_ATTACKSTOP );
    data << uint8(0xFF) << GetGUID();
    data << uint8(0xFF) << victimGuid;
    data << uint32( 0 );
    data << (uint32)0;

    SendMessageToSet(&data, true);
    sLog.outDetail("%u %X stopped attacking "I64FMT, GetGUIDLow(), GetGUIDHigh(), victimGuid);

    Creature *pVictim = ObjectAccessor::Instance().GetCreature(*this, victimGuid);
    if( pVictim != NULL )
        pVictim->AI().AttackStop(this);
}

uint16 Unit::GetDefenceSkillValue() const
{
    if(GetTypeId() == TYPEID_PLAYER)
        return ((Player*)this)->GetSkillValue (SKILL_DEFENSE);
    else
        return GetUnitMeleeSkill();
}

float Unit::GetUnitDodgeChance() const
{
    if(hasUnitState(UNIT_STAT_STUNDED))
        return 0;

    return GetTypeId() == TYPEID_PLAYER ? m_floatValues[ PLAYER_DODGE_PERCENTAGE ] : 5;
}

float Unit::GetUnitParryChance() const
{
    float chance = 0;
    if(GetTypeId() == TYPEID_PLAYER)
    {
        Player const* player = (Player const*)this;
                                                            // Parry passive skill
        if(player->HasSpell(SPELL_PASSIVE_PARRY_1) || player->HasSpell(SPELL_PASSIVE_PARRY_2))
        {
            Item *tmpitem = ((Player*)this)->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
            if(!tmpitem)
                tmpitem = ((Player*)this)->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);

            if(tmpitem && tmpitem->GetProto()->InventoryType == INVTYPE_WEAPON)
                chance = GetFloatValue(PLAYER_PARRY_PERCENTAGE);
        }
    }
    else if(GetTypeId() == TYPEID_UNIT)
    {
        if(((Creature const*)this)->GetCreatureInfo()->type == CREATURE_TYPE_HUMANOID)
            chance = 5;
    }

    return chance;
}

float Unit::GetUnitBlockChance() const
{
    float chance = 0;
    if(GetTypeId() == TYPEID_PLAYER)
    {
        Item *tmpitem = ((Player const*)this)->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        if(tmpitem && tmpitem->GetProto()->Block)
            chance = GetFloatValue(PLAYER_BLOCK_PERCENTAGE);
    }
    else
        chance = 5;

    return chance;
}

uint16 Unit::GetWeaponSkillValue (WeaponAttackType attType) const
{
    if(GetTypeId() == TYPEID_PLAYER)
    {
        uint16  slot;
        switch (attType)
        {
            case BASE_ATTACK: slot = EQUIPMENT_SLOT_MAINHAND; break;
            case OFF_ATTACK: slot = EQUIPMENT_SLOT_OFFHAND; break;
            case RANGED_ATTACK: slot = EQUIPMENT_SLOT_RANGED; break;
        }
        Item    *item = ((Player*)this)->GetItemByPos (INVENTORY_SLOT_BAG_0, slot);

        if(attType != EQUIPMENT_SLOT_MAINHAND && !item)
            return 0;

        uint32  skill = item ? item->GetSkill() : SKILL_UNARMED; // in range
        return ((Player*)this)->GetSkillValue (skill);
    }
    else
        return GetUnitMeleeSkill();
}

void Unit::_UpdateSpells( uint32 time )
{
    if(m_currentSpell != NULL)
    {
        m_currentSpell->update(time);
        if(m_currentSpell->IsAutoRepeat())
        {
            if(m_currentSpell->getState() == SPELL_STATE_FINISHED)
            {
                                                            //Auto shot
                if( m_currentSpell->m_spellInfo->Id == 75 && GetTypeId() == TYPEID_PLAYER )
                    resetAttackTimer( RANGED_ATTACK );
                else
                    setAttackTimer( RANGED_ATTACK, m_currentSpell->m_spellInfo->RecoveryTime);

                m_currentSpell->setState(SPELL_STATE_IDLE);
            }
            else if(m_currentSpell->getState() == SPELL_STATE_IDLE && isAttackReady(RANGED_ATTACK) )
            {
                // recheck range and req. items (ammo and gun, etc)
                if(m_currentSpell->CheckRange() == 0 && m_currentSpell->CheckItems() == 0 )
                {
                    m_currentSpell->setState(SPELL_STATE_PREPARING);
                    m_currentSpell->ReSetTimer();
                }
                else
                {
                    m_currentSpell->cancel();
                    delete m_currentSpell;
                    m_currentSpell = NULL;
                }
            }
        }
        else if(m_currentSpell->getState() == SPELL_STATE_FINISHED)
        {
            delete m_currentSpell;
            m_currentSpell = NULL;
        }
    }

    if(m_currentMeleeSpell != NULL)
    {
        m_currentMeleeSpell ->update(time);
        if(m_currentMeleeSpell ->getState() == SPELL_STATE_FINISHED)
        {
            delete m_currentMeleeSpell ;
            m_currentMeleeSpell  = NULL;
        }
    }

    // TODO: Find a better way to prevent crash when multiple auras are removed.
    m_removedAuras = 0;
    for (AuraMap::iterator i = m_Auras.begin(); i != m_Auras.end(); ++i)
        if ((*i).second)
            (*i).second->SetUpdated(false);

    for (AuraMap::iterator i = m_Auras.begin(), next; i != m_Auras.end(); i = next)
    {
        next = i;
        next++;
        if ((*i).second)
        {
            // prevent double update
            if ((*i).second->IsUpdated())
                continue;
            (*i).second->SetUpdated(true);
            (*i).second->Update( time );
            // several auras can be deleted due to update
            if (m_removedAuras)
            {
                if (m_Auras.empty()) break;
                next = m_Auras.begin();
                m_removedAuras = 0;
            }
        }
    }

    for (AuraMap::iterator i = m_Auras.begin(); i != m_Auras.end();)
    {
        if ((*i).second)
        {
            if ( !(*i).second->GetAuraDuration() && !(*i).second->IsPermanent() )
            {
                RemoveAura(i);
            }
            else
            {
                ++i;
            }
        }
        else
        {
            ++i;
        }
    }

    if(m_dynObj.empty())
        return;
    std::list<DynamicObject*>::iterator ite, dnext;
    for (ite = m_dynObj.begin(); ite != m_dynObj.end(); ite = dnext)
    {
        dnext = ite;
        dnext++;
        //(*i)->Update( difftime );
        if( (*ite)->isFinished() )
        {
            (*ite)->Delete();
            m_dynObj.erase(ite);
            if(m_dynObj.empty())
                break;
            else
                dnext = m_dynObj.begin();
        }
    }
    if(m_gameObj.empty())
        return;
    std::list<GameObject*>::iterator ite1, dnext1;
    for (ite1 = m_gameObj.begin(); ite1 != m_gameObj.end(); ite1 = dnext1)
    {
        dnext1 = ite1;
        dnext1++;
        //(*i)->Update( difftime );
        if( (*ite1)->isFinished() )
        {
            (*ite1)->Delete();
            m_gameObj.erase(ite1);
            if(m_gameObj.empty())
                break;
            else
                dnext1 = m_gameObj.begin();
        }
    }
}

void Unit::_UpdateHostil( uint32 time )
{
    if(!isInCombat() && m_hostilList.size() )
    {
        HostilList::iterator iter;
        for(iter=m_hostilList.begin(); iter!=m_hostilList.end(); ++iter)
        {
            iter->Hostility-=time/1000.0f;
            if(iter->Hostility<=0.0f)
            {
                m_hostilList.erase(iter);
                if(!m_hostilList.size())
                    break;
                else
                    iter = m_hostilList.begin();
            }
        }
    }
}

Unit* Unit::SelectHostilTarget()
{
    if(!m_hostilList.size())
        return NULL;

    m_hostilList.sort();
    m_hostilList.reverse();
    uint64 guid = m_hostilList.front().UnitGuid;
    if(guid != getVictim()->GetGUID())
        return ObjectAccessor::Instance().GetUnit(*this, guid);
    else
        return NULL;
}

void Unit::castSpell( Spell * pSpell )
{

    if(pSpell->IsMeleeSpell())
    {
        if(m_currentMeleeSpell)
        {
            m_currentMeleeSpell->cancel();
            delete m_currentMeleeSpell;
            m_currentMeleeSpell = NULL;
        }
        m_currentMeleeSpell = pSpell;
    }
    else
    {
        if(m_currentSpell)
        {
            m_currentSpell->cancel();
            delete m_currentSpell;
            m_currentSpell = NULL;
        }
        m_currentSpell = pSpell;
    }
}

void Unit::InterruptSpell()
{
    if(m_currentSpell)
    {
        //m_currentSpell->SendInterrupted(0x20);
        m_currentSpell->cancel();
    }
}

bool Unit::isInFront(Unit const* target, float radius)
{
    return GetDistanceSq(target)<=radius * radius && HasInArc( M_PI, target );
}

void Unit::SetInFront(Unit const* target)
{
    m_orientation = GetAngle(target);
}

void Unit::DeMorph()
{

    uint32 displayid = GetUInt32Value(UNIT_FIELD_NATIVEDISPLAYID);
    SetUInt32Value(UNIT_FIELD_DISPLAYID, displayid);
}

void Unit::DealWithSpellDamage(DynamicObject &obj)
{
    obj.DealWithSpellDamage(*this);
}

long Unit::GetTotalAuraModifier(uint32 ModifierID)
{
    uint32 modifier = 0;
    bool auraFound = false;

    AuraMap::const_iterator i;
    for (i = m_Auras.begin(); i != m_Auras.end(); i++)
    {
        if ((*i).second && (*i).second->GetModifier()->m_auraname == ModifierID)
        {
            auraFound = true;
            modifier += (*i).second->GetModifier()->m_amount;
        }
    }
    if (auraFound)
        modifier++;

    return modifier;
}

bool Unit::AddAura(Aura *Aur, bool uniq)
{
    AuraMap::iterator i = m_Auras.find( spellEffectPair(Aur->GetId(), Aur->GetEffIndex()) );
    // take out same spell
    if (i != m_Auras.end())
    {
        (*i).second->SetAuraDuration(Aur->GetAuraDuration());
        if ((*i).second->GetTarget())
            if ((*i).second->GetTarget()->GetTypeId() == TYPEID_PLAYER )
                (*i).second->UpdateAuraDuration();
        delete Aur;
    }
    else
    {
        if (!Aur->IsPassive())                              // passive auras stack with all
        {
            if (!RemoveNoStackAurasDueToAura(Aur))
            {
                delete Aur;
                return false;                               // couldnt remove conflicting aura with higher rank
            }
        }

        Aur->_AddAura();
        m_Auras[spellEffectPair(Aur->GetId(), Aur->GetEffIndex())] = Aur;
        if (Aur->GetModifier()->m_auraname < TOTAL_AURAS)
            m_modAuras[Aur->GetModifier()->m_auraname].push_back(Aur);
        m_AuraModifiers[Aur->GetModifier()->m_auraname] += (Aur->GetModifier()->m_amount);

        if (Aur->IsSingleTarget() && Aur->GetTarget() && Aur->GetSpellProto())
        {
            std::list<Aura *> *scAuras = Aur->GetCaster()->GetSingleCastAuras();
            std::list<Aura *>::iterator itr, next;
            for (itr = scAuras->begin(); itr != scAuras->end(); itr = next)
            {
                next = itr;
                next++;
                if ((*itr)->GetTarget() != Aur->GetTarget() &&
                    (*itr)->GetSpellProto()->Category == Aur->GetSpellProto()->Category &&
                    (*itr)->GetSpellProto()->SpellIconID == Aur->GetSpellProto()->SpellIconID &&
                    (*itr)->GetSpellProto()->SpellVisual == Aur->GetSpellProto()->SpellVisual &&
                    (*itr)->GetSpellProto()->Attributes == Aur->GetSpellProto()->Attributes &&
                    (*itr)->GetSpellProto()->AttributesEx == Aur->GetSpellProto()->AttributesEx &&
                    (*itr)->GetSpellProto()->AttributesExEx == Aur->GetSpellProto()->AttributesExEx)
                {
                    (*itr)->GetTarget()->RemoveAura((*itr)->GetId(), (*itr)->GetEffIndex());
                    if(scAuras->empty())
                        break;
                    else
                        next = scAuras->begin();
                }
            }
            scAuras->push_back(Aur);
        }
    }
    return true;
}

void Unit::RemoveRankAurasDueToSpell(uint32 spellId)
{
    SpellEntry *spellInfo = sSpellStore.LookupEntry(spellId);
    if(!spellInfo)
        return;
    AuraMap::iterator i,next;
    for (i = m_Auras.begin(); i != m_Auras.end(); i = next)
    {
        next = i;
        next++;
        uint32 i_spellId = (*i).second->GetId();
        if((*i).second && i_spellId && i_spellId != spellId)
        {
            if(IsRankSpellDueToSpell(spellInfo,i_spellId))
            {
                RemoveAurasDueToSpell(i_spellId);

                if( m_Auras.empty() )
                    break;
                else
                    next =  m_Auras.begin();
            }
        }
    }
}

bool Unit::RemoveNoStackAurasDueToAura(Aura *Aur)
{
    if (!Aur)
        return false;
    if (!Aur->GetSpellProto()) return false;
    uint32 spellId = Aur->GetId();
    uint32 effIndex = Aur->GetEffIndex();
    bool is_sec = IsSpellSingleEffectPerCaster(spellId);
    AuraMap::iterator i,next;
    for (i = m_Auras.begin(); i != m_Auras.end(); i = next)
    {
        next = i;
        next++;
        if (!(*i).second) continue;
        if (!(*i).second->GetSpellProto()) continue;
        if (IsPassiveSpell((*i).second->GetId())) continue;

        uint32 i_spellId = (*i).second->GetId();
        uint32 i_effIndex = (*i).second->GetEffIndex();
        if(i_spellId != spellId)
        {
            bool sec_match = false;
            if (is_sec && IsSpellSingleEffectPerCaster(i_spellId))
                if (Aur->GetCaster() == (*i).second->GetCaster())
                    if (GetSpellSpecific(spellId) == GetSpellSpecific(i_spellId))
                        sec_match = true;

            if(IsNoStackSpellDueToSpell(spellId, i_spellId) || sec_match)
            {
                // if sec_match this isnt always true, needs to be rechecked
                if (IsRankSpellDueToSpell(Aur->GetSpellProto(), i_spellId))
                    if(CompareAuraRanks(spellId, effIndex, i_spellId, i_effIndex) < 0)
                        return false;                       // cannot remove higher rank

                RemoveAurasDueToSpell(i_spellId);

                if( m_Auras.empty() )
                    break;
                else
                    next =  m_Auras.begin();
            }
            else                                            // Potions stack aura by aura
            if (Aur->GetSpellProto()->SpellFamilyName == SPELLFAMILY_POTION &&
                (*i).second->GetSpellProto()->SpellFamilyName == SPELLFAMILY_POTION)
            {
                if (IsNoStackAuraDueToAura(spellId, effIndex, i_spellId, i_effIndex))
                {
                    if(CompareAuraRanks(spellId, effIndex, i_spellId, i_effIndex) < 0)
                        return false;                       // cannot remove higher rank

                    RemoveAura(i);

                    if( m_Auras.empty() )
                        break;
                    else
                        next =  m_Auras.begin();
                }
            }
        }
    }
    return true;
}

void Unit::RemoveFirstAuraByDispel(uint32 dispel_type)
{
    AuraMap::iterator i;
    for (i = m_Auras.begin(); i != m_Auras.end(); ++i)
    {
        if ((*i).second && (*i).second->GetSpellProto()->Dispel == dispel_type)
        {
            if(dispel_type == 1)
            {
                bool positive = true;
                switch((*i).second->GetSpellProto()->EffectImplicitTargetA[(*i).second->GetEffIndex()])
                {
                    case TARGET_S_E:
                    case TARGET_AE_E:
                    case TARGET_AE_E_INSTANT:
                    case TARGET_AC_E:
                    case TARGET_INFRONT:
                    case TARGET_DUELVSPLAYER:
                    case TARGET_AE_E_CHANNEL:
                    case TARGET_AE_SELECTED:
                        positive = false;
                        break;

                    default:
                        positive = ((*i).second->GetSpellProto()->AttributesEx & (1<<7)) ? false : true;
                }
                if(positive)
                    continue;
            }
            break;
        }
    }

    if(i == m_Auras.end()) return;

    RemoveAura(i);
}

void Unit::RemoveAura(uint32 spellId, uint32 effindex)
{
    AuraMap::iterator i = m_Auras.find( spellEffectPair(spellId, effindex) );
    if(i != m_Auras.end())
        RemoveAura(i);
}

void Unit::RemoveAurasDueToSpell(uint32 spellId)
{
    for (int i = 0; i < 3; i++)
    {
        AuraMap::iterator iter = m_Auras.find(spellEffectPair(spellId, i));
        if (iter != m_Auras.end())
            RemoveAura(iter);
    }
}

void Unit::RemoveAura(AuraMap::iterator &i, bool onDeath)
{
    if ((*i).second->IsSingleTarget())
    {
        std::list<Aura *> *scAuras = (*i).second->GetCaster()->GetSingleCastAuras();
        scAuras->remove((*i).second);
    }
    // remove aura from party members when the caster turns off the aura
    if((*i).second->IsAreaAura())
    {
        Unit *i_caster = (*i).second->GetCaster(), *i_target = (*i).second->GetTarget();
        if(i_caster->GetTypeId() == TYPEID_PLAYER && i_caster->GetGUID() == i_target->GetGUID())
        {
            Group* pGroup = objmgr.GetGroupByLeader(((Player*)i_caster)->GetGroupLeader());
            float radius =  GetRadius(sSpellRadius.LookupEntry((*i).second->GetSpellProto()->EffectRadiusIndex[(*i).second->GetEffIndex()]));
            if(pGroup)
            {
                for(uint32 p=0;p<pGroup->GetMembersCount();p++)
                {
                    Unit* Target = ObjectAccessor::Instance().FindPlayer(pGroup->GetMemberGUID(p));
                    if(!Target || Target->GetGUID() == i_caster->GetGUID())
                        continue;
                    Aura *t_aura = Target->GetAura((*i).second->GetId(), (*i).second->GetEffIndex());
                    if (t_aura)
                        if (t_aura->GetCaster()->GetGUID() == i_caster->GetGUID())
                            Target->RemoveAura((*i).second->GetId(), (*i).second->GetEffIndex());
                }
            }
        }
    }
    m_AuraModifiers[(*i).second->GetModifier()->m_auraname] -= ((*i).second->GetModifier()->m_amount);
    if ((*i).second->GetModifier()->m_auraname < TOTAL_AURAS)
        m_modAuras[(*i).second->GetModifier()->m_auraname].remove((*i).second);
    (*i).second->SetRemoveOnDeath(onDeath);
    (*i).second->_RemoveAura();
    delete (*i).second;
    m_Auras.erase(i++);
    m_removedAuras++;                                       // internal count used by unit update
}

bool Unit::SetAurDuration(uint32 spellId, uint32 effindex,uint32 duration)
{
    AuraMap::iterator iter = m_Auras.find(spellEffectPair(spellId, effindex));
    if (iter != m_Auras.end())
    {
        (*iter).second->SetAuraDuration(duration);
        return true;
    }
    return false;
}

uint32 Unit::GetAurDuration(uint32 spellId, uint32 effindex)
{
    AuraMap::iterator iter = m_Auras.find(spellEffectPair(spellId, effindex));
    if (iter != m_Auras.end())
    {
        return (*iter).second->GetAuraDuration();
    }
    return 0;
}

void Unit::RemoveAllAuras()
{
    while (!m_Auras.empty())
    {
        AuraMap::iterator iter = m_Auras.begin();
        RemoveAura(iter);
    }
}

void Unit::RemoveAllAurasOnDeath()
{
    // used just after dieing to remove all visible auras
    // and disable the mods for the passive ones
    for(AuraMap::iterator iter = m_Auras.begin(); iter != m_Auras.end();)
        if (!iter->second->IsPassive())
            RemoveAura(iter, true);
    else
        ++iter;
    _RemoveAllAuraMods();
}

void Unit::_RemoveStatsMods()
{
    ApplyStats(false);
}

void Unit::_ApplyStatsMods()
{
    ApplyStats(true);
}

void Unit::ApplyStats(bool apply)
{
    // TODO:
    // -- add --
    // spell crit formula: 5 + INT/100
    // skill formula:  skill*0,04 for all, use defense skill for parry/dodge
    // froze spells gives + 50% change to crit

    if(GetTypeId() != TYPEID_PLAYER) return;

    PlayerCreateInfo* pinfo = ((Player*)this)->GetPlayerInfo();
    if(!pinfo) return;

    float val;
    uint32 val2,tem_att_power;

    // Armor
    val2 = 2*GetStat(STAT_AGILITY);

    ApplyArmorMod( val2, apply);

    // HP
    val2 = (GetStat(STAT_STAMINA) - pinfo->stamina)*10;

    ApplyMaxHealthMod( val2, apply);

    // MP
    if(getClass() != WARRIOR && getClass() != ROGUE)
    {
        val2 = (GetStat(STAT_INTELLECT) - pinfo->intellect)*15;

        ApplyMaxPowerMod(POWER_MANA, val2, apply);

    }

    float classrate = 0;

    // Melee Attack Power
    // && Melee DPS - (Damage Per Second)

    //Ranged
    if(getClass() == HUNTER)
        val2 = getLevel() * 2 + GetStat(STAT_AGILITY) * 2 - 20;
    else
        val2 = getLevel() + GetStat(STAT_AGILITY) * 2 - 20;

    if(!apply)
        tem_att_power = GetUInt32Value(UNIT_FIELD_RANGED_ATTACK_POWER) + GetUInt32Value(UNIT_FIELD_RANGED_ATTACK_POWER_MODS);

    ApplyModUInt32Value(UNIT_FIELD_RANGED_ATTACK_POWER, val2, apply);

    if(apply)
        tem_att_power = GetUInt32Value(UNIT_FIELD_RANGED_ATTACK_POWER) + GetUInt32Value(UNIT_FIELD_RANGED_ATTACK_POWER_MODS);

    val = GetFloatValue(UNIT_FIELD_RANGED_ATTACK_POWER_MULTIPLIER);
    if(val>0)
        tem_att_power = uint32(val*tem_att_power);

    val = tem_att_power/14.0f * GetAttackTime(RANGED_ATTACK)/1000;
    ApplyModFloatValue(UNIT_FIELD_MINRANGEDDAMAGE, val, apply);
    ApplyModFloatValue(UNIT_FIELD_MAXRANGEDDAMAGE, val, apply);

    //Not-ranged

    switch(getClass())
    {
        case WARRIOR: val2 = getLevel()*3 + GetStat(STAT_STRENGTH)*2 - 20; break;
        case PALADIN: val2 = getLevel()*3 + GetStat(STAT_STRENGTH)*2 - 20; break;
        case ROGUE:   val2 = getLevel()*2 + GetStat(STAT_STRENGTH) + GetStat(STAT_AGILITY) - 20; break;
        case HUNTER:  val2 = getLevel()*2 + GetStat(STAT_STRENGTH) + GetStat(STAT_AGILITY) - 20; break;
        case SHAMAN:  val2 = getLevel()*2 + GetStat(STAT_STRENGTH)*2 - 20; break;
        case DRUID:   val2 = GetStat(STAT_STRENGTH)*2 - 20; break;
        case MAGE:    val2 = GetStat(STAT_STRENGTH) - 10; break;
        case PRIEST:  val2 = GetStat(STAT_STRENGTH) - 10; break;
        case WARLOCK: val2 = GetStat(STAT_STRENGTH) - 10; break;
    }
    tem_att_power = GetUInt32Value(UNIT_FIELD_ATTACK_POWER) + GetUInt32Value(UNIT_FIELD_ATTACK_POWER_MODS);

    ApplyModUInt32Value(UNIT_FIELD_ATTACK_POWER, val2, apply);

    if(apply)
        tem_att_power = GetUInt32Value(UNIT_FIELD_ATTACK_POWER) + GetUInt32Value(UNIT_FIELD_ATTACK_POWER_MODS);

    val = GetFloatValue(UNIT_FIELD_ATTACK_POWER_MULTIPLIER);
    if(val>0)
        tem_att_power = uint32(val*tem_att_power);

    val = tem_att_power/14.0f * GetAttackTime(BASE_ATTACK)/1000;

    ApplyModFloatValue(UNIT_FIELD_MINDAMAGE, val, apply);
    ApplyModFloatValue(UNIT_FIELD_MAXDAMAGE, val, apply);

    val = tem_att_power/14.0f * GetAttackTime(OFF_ATTACK)/1000;

    ApplyModFloatValue(UNIT_FIELD_MINOFFHANDDAMAGE, val, apply);
    ApplyModFloatValue(UNIT_FIELD_MAXOFFHANDDAMAGE, val, apply);

    // critical
    if(getClass() == HUNTER) classrate = 53;
    else if(getClass() == ROGUE)  classrate = 29;
    else classrate = 20;

    val = float(5 + GetStat(STAT_AGILITY)/classrate);

    ApplyModFloatValue(PLAYER_CRIT_PERCENTAGE, val, apply);

    //dodge
    if(getClass() == HUNTER) classrate = 26.5;
    else if(getClass() == ROGUE)  classrate = 14.5;
    else classrate = 20;
                                                            ///*+(Defense*0,04);
    if (getRace() == NIGHTELF)
        val = float(GetStat(STAT_AGILITY)/classrate + 1);
    else
        val = float(GetStat(STAT_AGILITY)/classrate);

    ApplyModFloatValue(PLAYER_DODGE_PERCENTAGE, val, apply);

    //parry
    val = float(5);

    ApplyModFloatValue(PLAYER_PARRY_PERCENTAGE, val, apply);
}

void Unit::_RemoveAllAuraMods()
{
    for (AuraMap::iterator i = m_Auras.begin(); i != m_Auras.end(); ++i)
    {
        ApplyStats(false);
        (*i).second->ApplyModifier(false);
        ApplyStats(true);
    }
}

void Unit::_ApplyAllAuraMods()
{
    for (AuraMap::iterator i = m_Auras.begin(); i != m_Auras.end(); ++i)
    {
        ApplyStats(false);
        (*i).second->ApplyModifier(true);
        ApplyStats(true);
    }
}

// TODO: FIX-ME!!!
/*void Unit::_UpdateAura()
{
    if(GetTypeId() != TYPEID_PLAYER || !m_Auras)
        return;

    Player* pThis = (Player*)this;

    Player* pGroupGuy;
    Group* pGroup;

    pGroup = objmgr.GetGroupByLeader(pThis->GetGroupLeader());

    if(!SetAffDuration(m_Auras->GetId(),this,6000))
        AddAura(m_Auras);

    if(!pGroup)
        return;
    else
    {
        for(uint32 i=0;i<pGroup->GetMembersCount();i++)
        {
        pGroupGuy = ObjectAccessor::Instance().FindPlayer(pGroup->GetMemberGUID(i));

            if(!pGroupGuy)
                continue;
            if(pGroupGuy->GetGUID() == GetGUID())
                continue;
            if(sqrt(
                (GetPositionX()-pGroupGuy->GetPositionX())*(GetPositionX()-pGroupGuy->GetPositionX())
                +(GetPositionY()-pGroupGuy->GetPositionY())*(GetPositionY()-pGroupGuy->GetPositionY())
                +(GetPositionZ()-pGroupGuy->GetPositionZ())*(GetPositionZ()-pGroupGuy->GetPositionZ())
                ) <=30)
            {
                if(!pGroupGuy->SetAffDuration(m_Auras->GetId(),this,6000))
                pGroupGuy->AddAura(m_Auras);
            }
            else
            {
                if(m_removeAuraTimer == 0)
                {
                    printf("remove aura from %u\n", pGroupGuy->GetGUID());
                    pGroupGuy->RemoveAura(m_Auras->GetId());
                }
            }
        }
    }
    if(m_removeAuraTimer > 0)
        m_removeAuraTimer -= 1;
    else
        m_removeAuraTimer = 4;
}*/

Aura* Unit::GetAura(uint32 spellId, uint32 effindex)
{
    AuraMap::iterator iter = m_Auras.find(spellEffectPair(spellId, effindex));
    if (iter != m_Auras.end())
        return iter->second;
    return NULL;
}

float Unit::GetHostility(uint64 guid) const
{
    HostilList::const_iterator i;
    for ( i = m_hostilList.begin(); i!= m_hostilList.end(); i++)
    {
        if(i->UnitGuid==guid)
            return i->Hostility;
    }
    return 0.0f;
}

void Unit::AddHostil(uint64 guid, float hostility)
{
    HostilList::iterator i;
    for(i = m_hostilList.begin(); i != m_hostilList.end(); i++)
    {
        if(i->UnitGuid==guid)
        {
            i->Hostility+=hostility;
            return;
        }
    }
    m_hostilList.push_back(Hostil(guid,hostility));
}

void Unit::AddItemEnchant(Item *item,uint32 enchant_id,bool apply)
{
    if (GetTypeId() != TYPEID_PLAYER)
        return;

    if(!item)
        return;

    SpellItemEnchantment *pEnchant;
    pEnchant = sSpellItemEnchantmentStore.LookupEntry(enchant_id);
    if(!pEnchant)
        return;
    uint32 enchant_display = pEnchant->display_type;
    uint32 enchant_value1 = pEnchant->value1;
    //uint32 enchant_value2 = pEnchant->value2;
    uint32 enchant_spell_id = pEnchant->spellid;
    uint32 enchant_aura_id = pEnchant->aura_id;
    //uint32 enchant_description = pEnchant->description;
    uint32 enchant_slot = pEnchant->slot*3;
    SpellEntry *enchantSpell_info = sSpellStore.LookupEntry(enchant_spell_id);
    if(apply && enchant_id > 0)
    {
        for(int i=0;i<3;i++)
        item->SetUInt32Value(ITEM_FIELD_ENCHANTMENT+enchant_slot+i,0);
        item->SetUInt32Value(ITEM_FIELD_ENCHANTMENT+enchant_slot, enchant_id);
        //Add words before weapon name?
        //item->SetUInt32Value(ITEM_FIELD_ENCHANTMENT+enchant_slot+1, enchant_aura_id);
        //Charges for poison
        //item->SetUInt32Value(ITEM_FIELD_ENCHANTMENT+enchant_slot+2, charges);
    }

    if(enchant_display ==4)
    {
        ApplyArmorMod(enchant_value1,apply);
    }
    else if(enchant_display ==2)
    {
        if(getClass() == CLASS_HUNTER)
        {
            ApplyModUInt32Value(UNIT_FIELD_MINRANGEDDAMAGE,enchant_value1,apply);
            ApplyModUInt32Value(UNIT_FIELD_MAXRANGEDDAMAGE,enchant_value1,apply);
        }
        else
        {
            ApplyModUInt32Value(UNIT_FIELD_MINDAMAGE,enchant_value1,apply);
            ApplyModUInt32Value(UNIT_FIELD_MAXDAMAGE,enchant_value1,apply);
        }
    }
    else
    {
        if(apply && ((Player*)this)->IsItemSpellToEquip(enchantSpell_info))
        {
            Spell spell(this, enchantSpell_info, true, 0);
            SpellCastTargets targets;
            targets.setUnitTarget(this);
            spell.prepare(&targets);
        }
        else RemoveAurasDueToSpell(enchant_spell_id);
    }
}

void Unit::AddDynObject(DynamicObject* dynObj)
{
    m_dynObj.push_back(dynObj);
}

void Unit::RemoveDynObject(uint32 spellid)
{
    if(m_dynObj.empty())
        return;
    std::list<DynamicObject*>::iterator i, next;
    for (i = m_dynObj.begin(); i != m_dynObj.end(); i = next)
    {
        next = i;
        next++;
        if(spellid == 0 || (*i)->GetSpellId() == spellid)
        {
            (*i)->Delete();
            m_dynObj.erase(i);
            if(m_dynObj.empty())
                break;
            else
                next = m_dynObj.begin();
        }
    }
}

void Unit::AddGameObject(GameObject* gameObj)
{
    m_gameObj.push_back(gameObj);
}

void Unit::RemoveGameObject(uint32 spellid, bool del)
{
    if(m_gameObj.empty())
        return;
    std::list<GameObject*>::iterator i, next;
    for (i = m_gameObj.begin(); i != m_gameObj.end(); i = next)
    {
        next = i;
        next++;
        if(spellid == 0 || (*i)->GetSpellId() == spellid)
        {
            if(del)
                (*i)->Delete();

            m_gameObj.erase(i);
            if(m_gameObj.empty())
                break;
            else
                next = m_gameObj.begin();
        }
    }
}

void Unit::SendSpellNonMeleeDamageLog(uint64 targetGUID,uint32 SpellID,uint32 Damage, uint8 DamageType,uint32 AbsorbedDamage, uint32 Resist,bool PhysicalDamage, uint32 Blocked)
{
    WorldPacket data;
    data.Initialize(SMSG_SPELLNONMELEEDAMAGELOG);
    data << uint8(0xFF) << targetGUID;
    data << uint8(0xFF) << GetGUID();
    data << SpellID;
    data << Damage;
    data << DamageType;                                     //damagetype
    data << AbsorbedDamage;                                 //AbsorbedDamage
    data << Resist;                                         //resist
    data << (uint8)PhysicalDamage;
    data << uint8(0);
    data << Blocked;                                        //blocked
    data << uint8(0);
    SendMessageToSet( &data, true );
}

void Unit::SendAttackStateUpdate(uint32 HitInfo, uint64 targetGUID, uint8 SwingType, uint32 DamageType, uint32 Damage, uint32 AbsorbDamage, uint32 Resist, uint32 TargetState, uint32 BlockedAmount)
{
    sLog.outDebug("WORLD: Sending SMSG_ATTACKERSTATEUPDATE");

    WorldPacket data;
    data.Initialize(SMSG_ATTACKERSTATEUPDATE);
    data << (uint32)HitInfo;
    data << uint8(0xFF) << GetGUID();                       //source GUID
    data << uint8(0xFF) << targetGUID;                      //Target GUID
    data << (uint32)(Damage-AbsorbDamage);

    data << (uint8)SwingType;
    data << (uint32)DamageType;

    data << (float)Damage;                                  //
    data << (uint32)Damage;                                 // still need to double check damaga
    data << (uint32)AbsorbDamage;
    data << (uint32)Resist;
    data << (uint32)TargetState;

    if( AbsorbDamage == 0 )                                 //also 0x3E8 = 0x3E8, check when that happens
        data << (uint32)0;
    else
        data << (uint32)-1;

    data << (uint32)0;
    data << (uint32)BlockedAmount;

    SendMessageToSet( &data, true );
}

void Unit::setPowerType(Powers new_powertype)
{
    uint32 tem_bytes_0 = GetUInt32Value(UNIT_FIELD_BYTES_0);
    SetUInt32Value(UNIT_FIELD_BYTES_0,((tem_bytes_0<<8)>>8) + (uint32(new_powertype)<<24));
    switch(new_powertype)
    {
        default:
        case POWER_MANA:
            break;
        case POWER_RAGE:
            SetMaxPower(POWER_RAGE,1000);
            SetPower(   POWER_RAGE,0);
            break;
        case POWER_FOCUS:
            SetMaxPower(POWER_FOCUS,100);
            SetPower(   POWER_FOCUS,100);
            break;
        case POWER_ENERGY:
            SetMaxPower(POWER_ENERGY,100);
            SetPower(   POWER_ENERGY,100);
            break;
        case POWER_HAPPINESS:
            SetMaxPower(POWER_HAPPINESS,1000000);
            SetPower(POWER_HAPPINESS,1000000);
            break;
    }
}

FactionTemplateEntry* Unit::getFactionTemplateEntry() const
{
    FactionTemplateEntry* entry = sFactionTemplateStore.LookupEntry(getFaction());
    if(!entry)
    {
        static uint64 guid = 0;                             // prevent repeating spam same faction problem

        if(GetGUID() != guid)
        {
            if(GetTypeId() == TYPEID_PLAYER)
                sLog.outError("Player %s have invalide faction (fuction template id) #%u", ((Player*)this)->GetName(), getFaction());
            else
                sLog.outError("Creature (template id: %u) have invalide faction (fuction template id) #%u", ((Creature*)this)->GetCreatureInfo()->Entry, getFaction());
            guid = GetGUID();
        }
    }
    return entry;
}

bool Unit::Attack(Unit *victim)
{
    if(victim == this)
        return false;

    if (m_attacking)
    {
        if (m_attacking == victim)
            return false;
        AttackStop();
    }
    addUnitState(UNIT_STAT_ATTACKING);
    SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_ATTACKING);
    m_attacking = victim;
    m_attacking->_addAttacker(this);

    // delay offhand weapon attack to next attack time
    if(haveOffhandWeapon())
        resetAttackTimer(OFF_ATTACK);

    return true;
}

bool Unit::AttackStop()
{
    if (!m_attacking)
        return false;

    m_attacking->_removeAttacker(this);
    m_attacking = NULL;
    clearUnitState(UNIT_STAT_ATTACKING);
    RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_ATTACKING);
    return true;
}

bool Unit::isInCombatWithPlayer() const
{
    if(getVictim() && getVictim()->GetTypeId() == TYPEID_PLAYER)
        return true;

    for(AttackerSet::const_iterator i = m_attackers.begin(); i != m_attackers.end(); ++i)
    {
        if((*i)->GetTypeId() == TYPEID_PLAYER) return true;
    }
    return false;
}

void Unit::RemoveAllAttackers()
{
    while (m_attackers.size() != 0)
    {
        AttackerSet::iterator iter = m_attackers.begin();
        if(!(*iter)->AttackStop())
        {
            sLog.outError("WORLD: Unit has an attacker that isnt attacking it!");
            m_attackers.erase(iter);
        }
    }
}

void Unit::SetStateFlag(uint32 index, uint32 newFlag )
{
    index |= newFlag;
}

void Unit::RemoveStateFlag(uint32 index, uint32 oldFlag )
{
    index &= ~ oldFlag;
}

Creature* Unit::GetPet() const
{
    uint64 pet_guid = GetPetGUID();
    if(pet_guid)
    {
        Creature* pet = ObjectAccessor::Instance().GetCreature(*this, pet_guid);
        if(!pet)
        {
            sLog.outError("Unit::GetPet: Pet %u not exist.",GUID_LOPART(pet_guid));
            const_cast<Unit*>(this)->SetPet(0);
        }
        return pet;
    }
    else
        return NULL;
}

Creature* Unit::GetCharm() const
{
    uint64 charm_guid = GetCharmGUID();
    if(charm_guid)
    {
        Creature* pet = ObjectAccessor::Instance().GetCreature(*this, charm_guid);
        if(!pet)
        {
            sLog.outError("Unit::GetCharm: Charmed creature %u not exist.",GUID_LOPART(charm_guid));
            const_cast<Unit*>(this)->SetCharm(0);
        }
        return pet;
    }
    else
        return NULL;
}

void Unit::SetPet(Creature* pet)
{
    SetUInt64Value(UNIT_FIELD_SUMMON,pet ? pet->GetGUID() : 0);
}

void Unit::SetCharm(Creature* charmed)
{
    SetUInt64Value(UNIT_FIELD_CHARM,charmed ? charmed->GetGUID() : 0);
}

void Unit::UnsummonTotem(int8 slot)
{
    uint64 t_guids[4] = { m_TotemSlot1, m_TotemSlot2, m_TotemSlot3, m_TotemSlot4 };
    WorldPacket data;

    for (int8 i = 0; i < 4; i++)
    {
        if (i != slot && slot != -1) continue;
        Creature *OldTotem = ObjectAccessor::Instance().GetCreature(*this, t_guids[i]);

        if(OldTotem)
        {
            data.Initialize(SMSG_GAMEOBJECT_DESPAWN_ANIM);
            data << t_guids[i];
            SendMessageToSet(&data, true);

            data.Initialize(SMSG_DESTROY_OBJECT);
            data << t_guids[i];
            SendMessageToSet(&data, true);
            MapManager::Instance().GetMap(OldTotem->GetMapId())->Remove(OldTotem, true);
            OldTotem = NULL;
        }
    }
}

void Unit::SendHealSpellOnPlayer(Unit *pVictim, uint32 SpellID, uint32 Damage)
{
    WorldPacket data;
    data.Initialize(SMSG_HEALSPELL_ON_PLAYER_OBSOLETE);
    data << uint8(0xFF) << pVictim->GetGUID();
    data << uint8(0xFF) << GetGUID();
    data << SpellID;
    data << Damage;
    data << uint8(0);
    SendMessageToSet(&data, true);
}

void Unit::SendHealSpellOnPlayerPet(Unit *pVictim, uint32 SpellID, uint32 Damage)
{
    WorldPacket data;
    data.Initialize(SMSG_HEALSPELL_ON_PLAYERS_PET_OBSOLETE);
    data << uint8(0xFF) << pVictim->GetGUID();
    data << uint8(0xFF) << GetGUID();
    data << SpellID;
    data << Damage;
    data << uint8(0);
    SendMessageToSet(&data, true);
}

uint32 Unit::SpellDamageBonus(Unit *pVictim, SpellEntry *spellProto, uint32 pdamage)
{
    if(!spellProto || !pVictim) return pdamage;
    //If m_immuneToDamage type contain this damage type, IMMUNE damage.
    for (SpellImmuneList::iterator itr = pVictim->m_spellImmune[IMMUNITY_DAMAGE].begin(), next; itr != pVictim->m_spellImmune[IMMUNITY_DAMAGE].end(); itr = next)
    {
        next = itr;
        next++;
        if((*itr)->type & uint32(1<<spellProto->School))
        {
            pdamage = 0;
            break;
        }
    }
    //If m_immuneToSchool type contain this school type, IMMUNE damage.
    for (SpellImmuneList::iterator itr = pVictim->m_spellImmune[IMMUNITY_SCHOOL].begin(), next; itr != pVictim->m_spellImmune[IMMUNITY_SCHOOL].end(); itr = next)
    {
        next = itr;
        next++;
        if((*itr)->type & uint32(1<<spellProto->School))
        {
            pdamage = 0;
            break;
        }
    }
    if(pdamage == 0)
        return pdamage;
    CreatureInfo *cinfo = NULL;
    if(pVictim->GetTypeId() != TYPEID_PLAYER)
        cinfo = ((Creature*)pVictim)->GetCreatureInfo();

    // Damage Done
    int32 AdvertisedBenefit = 0;
    uint32 PenaltyFactor = 0;
    uint32 CastingTime = GetCastTime(sCastTime.LookupEntry(spellProto->CastingTimeIndex));
    if (CastingTime > 3500) CastingTime = 3500;
    if (CastingTime < 1500) CastingTime = 1500;

    AuraList& mDamageDoneCreature = GetAurasByType(SPELL_AURA_MOD_DAMAGE_DONE_CREATURE);
    for(AuraList::iterator i = mDamageDoneCreature.begin();i != mDamageDoneCreature.end(); ++i)
        if(cinfo && (cinfo->type & (*i)->GetModifier()->m_miscvalue) != 0)
            AdvertisedBenefit += (*i)->GetModifier()->m_amount;

    AuraList& mDamageDone = GetAurasByType(SPELL_AURA_MOD_DAMAGE_DONE);
    for(AuraList::iterator i = mDamageDone.begin();i != mDamageDone.end(); ++i)
        if(((*i)->GetModifier()->m_miscvalue & (int32)(1<<spellProto->School)) != 0)
            AdvertisedBenefit += (*i)->GetModifier()->m_amount;

    AuraList& mDamageTaken = pVictim->GetAurasByType(SPELL_AURA_MOD_DAMAGE_TAKEN);
    for(AuraList::iterator i = mDamageTaken.begin();i != mDamageTaken.end(); ++i)
        if(((*i)->GetModifier()->m_miscvalue & (int32)(1<<spellProto->School)) != 0)
            AdvertisedBenefit += (*i)->GetModifier()->m_amount;

    // TODO - fix PenaltyFactor and complete the formula from the wiki
    float ActualBenefit = (float)AdvertisedBenefit * ((float)CastingTime / 3500) * (float)(100 - PenaltyFactor) / 100;
    pdamage += uint32(ActualBenefit);

    // Spell Criticals
    bool crit = false;
    int32 critchance = m_baseSpellCritChance + int32(GetStat(STAT_INTELLECT)/100-1);
    critchance = critchance > 0 ? critchance :0;

    if (GetTypeId() == TYPEID_PLAYER)
        ((Player*)this)->ApplySpellMod(spellProto->Id, SPELLMOD_CRITICAL_CHANCE, critchance);

    AuraList& mSpellCritSchool = GetAurasByType(SPELL_AURA_MOD_SPELL_CRIT_CHANCE_SCHOOL);
    for(AuraList::iterator i = mSpellCritSchool.begin(); i != mSpellCritSchool.end(); ++i)
        if((*i)->GetModifier()->m_miscvalue == -2 || ((*i)->GetModifier()->m_miscvalue & (int32)(1<<spellProto->School)) != 0)
            critchance += (*i)->GetModifier()->m_amount;

    critchance = critchance > 0 ? critchance :0;
    if(critchance >= urand(0,100))
    {
        pdamage = uint32(pdamage*1.5);
        crit = true;
    }

    return pdamage;
}

uint32 Unit::MeleeDamageBonus(Unit *pVictim, uint32 pdamage)
{
    if(!pVictim) return pdamage;
    //If m_immuneToDamage type contain magic, IMMUNE damage.
    for (SpellImmuneList::iterator itr = pVictim->m_spellImmune[IMMUNITY_DAMAGE].begin(), next; itr != pVictim->m_spellImmune[IMMUNITY_DAMAGE].end(); itr = next)
    {
        next = itr;
        next++;
        if((*itr)->type & IMMUNE_DAMAGE_PHYSICAL)
        {
            pdamage = 0;
            break;
        }
    }
    //If m_immuneToSchool type contain this school type, IMMUNE damage.
    for (SpellImmuneList::iterator itr = pVictim->m_spellImmune[IMMUNITY_SCHOOL].begin(), next; itr != pVictim->m_spellImmune[IMMUNITY_SCHOOL].end(); itr = next)
    {
        next = itr;
        next++;
        if((*itr)->type & IMMUNE_SCHOOL_PHYSICAL)
        {
            pdamage = 0;
            break;
        }
    }
    if(pdamage == 0)
        return pdamage;
    CreatureInfo *cinfo = NULL;
    if(pVictim->GetTypeId() != TYPEID_PLAYER)
        cinfo = ((Creature*)pVictim)->GetCreatureInfo();
    if(GetTypeId() != TYPEID_PLAYER && ((Creature*)this)->isPet())
    {
        if(getPowerType() == POWER_FOCUS)
        {
            uint32 happiness = GetPower(POWER_HAPPINESS);
            if(happiness>=750000)
                pdamage = uint32(pdamage*1.25);
            else if(happiness>=500000)
                pdamage = uint32(pdamage*1.0);
            else pdamage = uint32(pdamage*0.75);
        }
    }

    AuraList& mDamageDoneCreature = GetAurasByType(SPELL_AURA_MOD_DAMAGE_DONE_CREATURE);
    for(AuraList::iterator i = mDamageDoneCreature.begin();i != mDamageDoneCreature.end(); ++i)
        if(cinfo && cinfo->type == uint32((*i)->GetModifier()->m_miscvalue))
            pdamage += (*i)->GetModifier()->m_amount;

    AuraList& mDamageDone = GetAurasByType(SPELL_AURA_MOD_DAMAGE_DONE);
    for(AuraList::iterator i = mDamageDone.begin();i != mDamageDone.end(); ++i)
        if((*i)->GetModifier()->m_miscvalue & IMMUNE_SCHOOL_PHYSICAL)
            pdamage += (*i)->GetModifier()->m_amount;

    AuraList& mDamageTaken = GetAurasByType(SPELL_AURA_MOD_DAMAGE_TAKEN);
    for(AuraList::iterator i = mDamageTaken.begin();i != mDamageTaken.end(); ++i)
        if((*i)->GetModifier()->m_miscvalue & IMMUNE_SCHOOL_PHYSICAL)
            pdamage += (*i)->GetModifier()->m_amount;

    AuraList& mCreatureAttackPower = GetAurasByType(SPELL_AURA_MOD_CREATURE_ATTACK_POWER);
    for(AuraList::iterator i = mCreatureAttackPower.begin();i != mCreatureAttackPower.end(); ++i)
        if(cinfo && (cinfo->type & uint32((*i)->GetModifier()->m_miscvalue)) != 0)
            pdamage += uint32((*i)->GetModifier()->m_amount/14.0f * GetAttackTime(BASE_ATTACK)/1000);

    pdamage = uint32(pdamage * (m_modDamagePCT+100)/100);
    return pdamage;
}

void Unit::ApplySpellImmune(uint32 spellId, uint32 op, uint32 type, bool apply)
{
    if (apply)
    {
        for (SpellImmuneList::iterator itr = m_spellImmune[op].begin(), next; itr != m_spellImmune[op].end(); itr = next)
        {
            next = itr;
            next++;
            if((*itr)->type == type)
            {
                m_spellImmune[op].erase(itr);
                if (m_spellImmune[op].empty())
                    break;
                else
                    next = m_spellImmune[op].begin();
            }
        }
        SpellImmune *Immune = new SpellImmune();
        Immune->spellId = spellId;
        Immune->type = type;
        m_spellImmune[op].push_back(Immune);
    }
    else
    {
        for (SpellImmuneList::iterator itr = m_spellImmune[op].begin(), next; itr != m_spellImmune[op].end(); itr = next)
        {
            next = itr;
            next++;
            if((*itr)->spellId == spellId)
            {
                m_spellImmune[op].erase(itr);
                break;
            }
        }
    }

}