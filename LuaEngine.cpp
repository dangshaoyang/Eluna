/*
* Copyright (C) 2010 - 2014 Eluna Lua Engine <http://emudevs.com/>
* This program is free software licensed under GPL version 3
* Please see the included DOCS/LICENSE.md for more information
*/

#include <ace/Dirent.h>
#include <ace/OS_NS_sys_stat.h>
#include "HookMgr.h"
#include "LuaEngine.h"
#include "Includes.h"

Eluna::ScriptPaths Eluna::scripts;
Eluna* Eluna::GEluna = NULL;
bool Eluna::reload = false;

extern void RegisterFunctions(lua_State* L);

void Eluna::Initialize()
{
    uint32 oldMSTime = GetCurrTime();

    scripts.clear();

    std::string folderpath = ConfigMgr::GetStringDefault("Eluna.ScriptPath", "lua_scripts");
#if PLATFORM == PLATFORM_UNIX || PLATFORM == PLATFORM_APPLE
    if (folderpath[0] == '~')
        if (const char* home = getenv("HOME"))
            folderpath.replace(0, 1, home);
#endif
    ELUNA_LOG_INFO("[Eluna]: Searching scripts from `%s`", folderpath.c_str());
    GetScripts(folderpath, scripts);
    GetScripts(folderpath + "/extensions", scripts);

    ELUNA_LOG_INFO("[Eluna]: Loaded %u scripts in %u ms", uint32(scripts.size()), GetTimeDiff(oldMSTime));

    // Create global eluna
    new Eluna();
}

void Eluna::Uninitialize()
{
    delete GEluna;
    scripts.clear();
}

void Eluna::ReloadEluna()
{
    eWorld->SendServerMessage(SERVER_MSG_STRING, "Reloading Eluna...");
    Uninitialize();
    Initialize();

    reload = false;
}

Eluna::Eluna():
L(luaL_newstate()),

m_EventMgr(new EventMgr(*this)),

ServerEventBindings(new EventBind<HookMgr::ServerEvents>("ServerEvents", *this)),
PlayerEventBindings(new EventBind<HookMgr::PlayerEvents>("PlayerEvents", *this)),
GuildEventBindings(new EventBind<HookMgr::GuildEvents>("GuildEvents", *this)),
GroupEventBindings(new EventBind<HookMgr::GroupEvents>("GroupEvents", *this)),
VehicleEventBindings(new EventBind<HookMgr::VehicleEvents>("VehicleEvents", *this)),

PacketEventBindings(new EntryBind<HookMgr::PacketEvents>("PacketEvents", *this)),
CreatureEventBindings(new EntryBind<HookMgr::CreatureEvents>("CreatureEvents", *this)),
CreatureGossipBindings(new EntryBind<HookMgr::GossipEvents>("GossipEvents (creature)", *this)),
GameObjectEventBindings(new EntryBind<HookMgr::GameObjectEvents>("GameObjectEvents", *this)),
GameObjectGossipBindings(new EntryBind<HookMgr::GossipEvents>("GossipEvents (gameobject)", *this)),
ItemEventBindings(new EntryBind<HookMgr::ItemEvents>("ItemEvents", *this)),
ItemGossipBindings(new EntryBind<HookMgr::GossipEvents>("GossipEvents (item)", *this)),
playerGossipBindings(new EntryBind<HookMgr::GossipEvents>("GossipEvents (player)", *this))
{
    // open base lua
    luaL_openlibs(L);
    RegisterFunctions(L);

    // Create hidden table with weak values
    lua_newtable(L);
    lua_newtable(L);
    lua_pushstring(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    userdata_table = luaL_ref(L, LUA_REGISTRYINDEX);

    // Replace this with map insert if making multithread version
    ASSERT(!Eluna::GEluna);
    Eluna::GEluna = this;

    // run scripts
    RunScripts(scripts);
}

Eluna::~Eluna()
{
    OnLuaStateClose();

    // Replace this with map remove if making multithread version
    Eluna::GEluna = NULL;

    delete m_EventMgr;

    delete ServerEventBindings;
    delete PlayerEventBindings;
    delete GuildEventBindings;
    delete GroupEventBindings;
    delete VehicleEventBindings;

    delete PacketEventBindings;
    delete CreatureEventBindings;
    delete CreatureGossipBindings;
    delete GameObjectEventBindings;
    delete GameObjectGossipBindings;
    delete ItemEventBindings;
    delete ItemGossipBindings;
    delete playerGossipBindings;

    // Must close lua state after deleting stores and mgr
    lua_close(L);
}

// Finds lua script files from given path (including subdirectories) and pushes them to scripts
void Eluna::GetScripts(std::string path, ScriptPaths& scripts)
{
    ELUNA_LOG_DEBUG("[Eluna]: GetScripts from path `%s`", path.c_str());

    ACE_Dirent dir;
    if (dir.open(path.c_str()) == -1)
    {
        ELUNA_LOG_ERROR("[Eluna]: Error No `%s` directory found, creating it", path.c_str());
        ACE_OS::mkdir(path.c_str());
        return;
    }

    ACE_DIRENT *directory = 0;
    while ((directory = dir.read()))
    {
        // Skip the ".." and "." files.
        if (ACE::isdotdir(directory->d_name))
            continue;

        std::string fullpath = path + "/" + directory->d_name;

        ACE_stat stat_buf;
        if (ACE_OS::lstat(fullpath.c_str(), &stat_buf) == -1)
            continue;

        // load subfolder
        if ((stat_buf.st_mode & S_IFMT) == (S_IFDIR))
        {
            GetScripts(fullpath, scripts);
            continue;
        }

        // was file, check extension
        ELUNA_LOG_DEBUG("[Eluna]: GetScripts Checking file `%s`", fullpath.c_str());
        std::string ext = fullpath.substr(fullpath.length() - 4, 4);
        if (ext != ".lua" && ext != ".dll")
            continue;

        // was correct, add path to scripts to load
        ELUNA_LOG_DEBUG("[Eluna]: GetScripts add path `%s`", fullpath.c_str());
        scripts.erase(fullpath);
        scripts.insert(fullpath);
    }
}

void Eluna::RunScripts(ScriptPaths& scripts)
{
    uint32 count = 0;
    // load last first to load extensions first
    for (ScriptPaths::const_reverse_iterator it = scripts.rbegin(); it != scripts.rend(); ++it)
    {
        if (!luaL_loadfile(L, it->c_str()) && !lua_pcall(L, 0, 0, 0))
        {
            // successfully loaded and ran file
            ELUNA_LOG_DEBUG("[Eluna]: Successfully loaded `%s`", it->c_str());
            ++count;
            continue;
        }
        ELUNA_LOG_ERROR("[Eluna]: Error loading file `%s`", it->c_str());
        report(L);
    }
    ELUNA_LOG_DEBUG("[Eluna]: Loaded %u Lua scripts", count);
}

void Eluna::RemoveRef(const void* obj)
{
    lua_rawgeti(sEluna->L, LUA_REGISTRYINDEX, sEluna->userdata_table);
    lua_pushfstring(sEluna->L, "%p", obj);
    lua_gettable(sEluna->L, -2);
    if (!lua_isnoneornil(sEluna->L, -1))
    {
        lua_pushfstring(sEluna->L, "%p", obj);
        lua_pushnil(sEluna->L);
        lua_settable(sEluna->L, -4);
    }
    lua_pop(sEluna->L, 2);
}

void Eluna::report(lua_State* L)
{
    const char* msg = lua_tostring(L, -1);
    while (msg)
    {
        lua_pop(L, 1);
        ELUNA_LOG_ERROR("%s", msg);
        msg = lua_tostring(L, -1);
    }
}

void Eluna::ExecuteCall(lua_State* L, int params, int res)
{
    int top = lua_gettop(L);
    luaL_checktype(L, top - params, LUA_TFUNCTION);
    if (lua_pcall(L, params, res, 0))
        report(L);
}

void Eluna::Push(lua_State* L)
{
    lua_pushnil(L);
}
void Eluna::Push(lua_State* L, const uint64 l)
{
    std::ostringstream ss;
    ss << l;
    Push(L, ss.str());
}
void Eluna::Push(lua_State* L, const int64 l)
{
    std::ostringstream ss;
    ss << l;
    Push(L, ss.str());
}
void Eluna::Push(lua_State* L, const uint32 u)
{
    lua_pushunsigned(L, u);
}
void Eluna::Push(lua_State* L, const int32 i)
{
    lua_pushinteger(L, i);
}
void Eluna::Push(lua_State* L, const double d)
{
    lua_pushnumber(L, d);
}
void Eluna::Push(lua_State* L, const float f)
{
    lua_pushnumber(L, f);
}
void Eluna::Push(lua_State* L, const bool b)
{
    lua_pushboolean(L, b);
}
void Eluna::Push(lua_State* L, const std::string str)
{
    lua_pushstring(L, str.c_str());
}
void Eluna::Push(lua_State* L, const char* str)
{
    lua_pushstring(L, str);
}
void Eluna::Push(lua_State* L, Pet const* pet)
{
    Push(L, pet->ToCreature());
}
void Eluna::Push(lua_State* L, TempSummon const* summon)
{
    Push(L, summon->ToCreature());
}
void Eluna::Push(lua_State* L, Unit const* unit)
{
    if (!unit)
    {
        Push(L);
        return;
    }
    switch (unit->GetTypeId())
    {
    case TYPEID_UNIT:
        Push(L, unit->ToCreature());
        break;
    case TYPEID_PLAYER:
        Push(L, unit->ToPlayer());
        break;
    default:
        ElunaTemplate<Unit>::push(L, unit);
    }
}
void Eluna::Push(lua_State* L, WorldObject const* obj)
{
    if (!obj)
    {
        Push(L);
        return;
    }
    switch (obj->GetTypeId())
    {
    case TYPEID_UNIT:
        Push(L, obj->ToCreature());
        break;
    case TYPEID_PLAYER:
        Push(L, obj->ToPlayer());
        break;
    case TYPEID_GAMEOBJECT:
        Push(L, obj->ToGameObject());
        break;
    case TYPEID_CORPSE:
        Push(L, obj->ToCorpse());
        break;
    default:
        ElunaTemplate<WorldObject>::push(L, obj);
    }
}
void Eluna::Push(lua_State* L, Object const* obj)
{
    if (!obj)
    {
        Push(L);
        return;
    }
    switch (obj->GetTypeId())
    {
    case TYPEID_UNIT:
        Push(L, obj->ToCreature());
        break;
    case TYPEID_PLAYER:
        Push(L, obj->ToPlayer());
        break;
    case TYPEID_GAMEOBJECT:
        Push(L, obj->ToGameObject());
        break;
    case TYPEID_CORPSE:
        Push(L, obj->ToCorpse());
        break;
    default:
        ElunaTemplate<Object>::push(L, obj);
    }
}
template<> bool Eluna::CHECKVAL<bool>(lua_State* L, int narg)
{
    return lua_isnumber(L, narg) ? luaL_optnumber(L, narg, 1) ? true : false : lua_toboolean(L, narg);
}
template<> bool Eluna::CHECKVAL<bool>(lua_State* L, int narg, bool def)
{
    return lua_isnone(L, narg) ? def : lua_isnumber(L, narg) ? luaL_optnumber(L, narg, 1) ? true : false : lua_toboolean(L, narg);
}
template<> float Eluna::CHECKVAL<float>(lua_State* L, int narg)
{
    return luaL_checknumber(L, narg);
}
template<> float Eluna::CHECKVAL<float>(lua_State* L, int narg, float def)
{
    if (lua_isnoneornil(L, narg) || !lua_isnumber(L, narg))
        return def;
    return luaL_optnumber(L, narg, def);
}
template<> double Eluna::CHECKVAL<double>(lua_State* L, int narg)
{
    return luaL_checknumber(L, narg);
}
template<> double Eluna::CHECKVAL<double>(lua_State* L, int narg, double def)
{
    if (lua_isnoneornil(L, narg) || !lua_isnumber(L, narg))
        return def;
    return luaL_optnumber(L, narg, def);
}
template<> int8 Eluna::CHECKVAL<int8>(lua_State* L, int narg)
{
    return luaL_checkint(L, narg);
}
template<> int8 Eluna::CHECKVAL<int8>(lua_State* L, int narg, int8 def)
{
    if (lua_isnoneornil(L, narg) || !lua_isnumber(L, narg))
        return def;
    return luaL_optint(L, narg, def);
}
template<> uint8 Eluna::CHECKVAL<uint8>(lua_State* L, int narg)
{
    return luaL_checkunsigned(L, narg);
}
template<> uint8 Eluna::CHECKVAL<uint8>(lua_State* L, int narg, uint8 def)
{
    if (lua_isnoneornil(L, narg) || !lua_isnumber(L, narg))
        return def;
    return luaL_optunsigned(L, narg, def);
}
template<> int16 Eluna::CHECKVAL<int16>(lua_State* L, int narg)
{
    return luaL_checkint(L, narg);
}
template<> int16 Eluna::CHECKVAL<int16>(lua_State* L, int narg, int16 def)
{
    if (lua_isnoneornil(L, narg) || !lua_isnumber(L, narg))
        return def;
    return luaL_optint(L, narg, def);
}
template<> uint16 Eluna::CHECKVAL<uint16>(lua_State* L, int narg)
{
    return luaL_checkunsigned(L, narg);
}
template<> uint16 Eluna::CHECKVAL<uint16>(lua_State* L, int narg, uint16 def)
{
    if (lua_isnoneornil(L, narg) || !lua_isnumber(L, narg))
        return def;
    return luaL_optunsigned(L, narg, def);
}
template<> uint32 Eluna::CHECKVAL<uint32>(lua_State* L, int narg)
{
    return luaL_checkunsigned(L, narg);
}
template<> uint32 Eluna::CHECKVAL<uint32>(lua_State* L, int narg, uint32 def)
{
    if (lua_isnoneornil(L, narg) || !lua_isnumber(L, narg))
        return def;
    return luaL_optunsigned(L, narg, def);
}
template<> int32 Eluna::CHECKVAL<int32>(lua_State* L, int narg)
{
    return luaL_checklong(L, narg);
}
template<> int32 Eluna::CHECKVAL<int32>(lua_State* L, int narg, int32 def)
{
    if (lua_isnoneornil(L, narg) || !lua_isnumber(L, narg))
        return def;
    return luaL_optlong(L, narg, def);
}
template<> const char* Eluna::CHECKVAL<const char*>(lua_State* L, int narg)
{
    return luaL_checkstring(L, narg);
}
template<> const char* Eluna::CHECKVAL<const char*>(lua_State* L, int narg, const char* def)
{
    if (lua_isnoneornil(L, narg) || !lua_isstring(L, narg))
        return def;
    return luaL_optstring(L, narg, def);
}
template<> std::string Eluna::CHECKVAL<std::string>(lua_State* L, int narg)
{
    return luaL_checkstring(L, narg);
}
template<> std::string Eluna::CHECKVAL<std::string>(lua_State* L, int narg, std::string def)
{
    if (lua_isnoneornil(L, narg) || !lua_isstring(L, narg))
        return def;
    return luaL_optstring(L, narg, def.c_str());
}
template<> uint64 Eluna::CHECKVAL<uint64>(lua_State* L, int narg)
{
    const char* c_str = CHECKVAL<const char*>(L, narg, NULL);
    if (!c_str)
        return luaL_argerror(L, narg, "uint64 (as string) expected");
    uint64 l = 0;
    sscanf(c_str, UI64FMTD, &l);
    return l;
}
template<> uint64 Eluna::CHECKVAL<uint64>(lua_State* L, int narg, uint64 def)
{
    const char* c_str = CHECKVAL<const char*>(L, narg, NULL);
    if (!c_str)
        return def;
    uint64 l = 0;
    sscanf(c_str, UI64FMTD, &l);
    return l;
}
template<> int64 Eluna::CHECKVAL<int64>(lua_State* L, int narg)
{
    const char* c_str = CHECKVAL<const char*>(L, narg, NULL);
    if (!c_str)
        return luaL_argerror(L, narg, "int64 (as string) expected");
    int64 l = 0;
    sscanf(c_str, SI64FMTD, &l);
    return l;
}
template<> int64 Eluna::CHECKVAL<int64>(lua_State* L, int narg, int64 def)
{
    const char* c_str = CHECKVAL<const char*>(L, narg, NULL);
    if (!c_str)
        return def;
    int64 l = 0;
    sscanf(c_str, SI64FMTD, &l);
    return l;
}
#define TEST_OBJ(T, O, E, F)\
{\
    if (!O || !O->F())\
    {\
    if (E)\
    {\
    std::string errmsg(ElunaTemplate<T>::tname);\
    errmsg += " expected";\
    luaL_argerror(L, narg, errmsg.c_str());\
    }\
    return NULL;\
    }\
    return O->F();\
}
template<> Unit* Eluna::CHECKOBJ<Unit>(lua_State* L, int narg, bool error)
{
    WorldObject* obj = CHECKOBJ<WorldObject>(L, narg, false);
    TEST_OBJ(Unit, obj, error, ToUnit);
}
template<> Player* Eluna::CHECKOBJ<Player>(lua_State* L, int narg, bool error)
{
    WorldObject* obj = CHECKOBJ<WorldObject>(L, narg, false);
    TEST_OBJ(Player, obj, error, ToPlayer);
}
template<> Creature* Eluna::CHECKOBJ<Creature>(lua_State* L, int narg, bool error)
{
    WorldObject* obj = CHECKOBJ<WorldObject>(L, narg, false);
    TEST_OBJ(Creature, obj, error, ToCreature);
}
template<> GameObject* Eluna::CHECKOBJ<GameObject>(lua_State* L, int narg, bool error)
{
    WorldObject* obj = CHECKOBJ<WorldObject>(L, narg, false);
    TEST_OBJ(GameObject, obj, error, ToGameObject);
}
template<> Corpse* Eluna::CHECKOBJ<Corpse>(lua_State* L, int narg, bool error)
{
    WorldObject* obj = CHECKOBJ<WorldObject>(L, narg, false);
    TEST_OBJ(Corpse, obj, error, ToCorpse);
}
#undef TEST_OBJ

// Saves the function reference ID given to the register type's store for given entry under the given event
void Eluna::Register(uint8 regtype, uint32 id, uint32 evt, int functionRef)
{
    switch (regtype)
    {
    case HookMgr::REGTYPE_SERVER:
        if (evt < HookMgr::SERVER_EVENT_COUNT)
        {
            ServerEventBindings->Insert(evt, functionRef);
            return;
        }
        break;

    case HookMgr::REGTYPE_PLAYER:
        if (evt < HookMgr::PLAYER_EVENT_COUNT)
        {
            PlayerEventBindings->Insert(evt, functionRef);
            return;
        }
        break;

    case HookMgr::REGTYPE_GUILD:
        if (evt < HookMgr::GUILD_EVENT_COUNT)
        {
            GuildEventBindings->Insert(evt, functionRef);
            return;
        }
        break;

    case HookMgr::REGTYPE_GROUP:
        if (evt < HookMgr::GROUP_EVENT_COUNT)
        {
            GroupEventBindings->Insert(evt, functionRef);
            return;
        }
        break;

    case HookMgr::REGTYPE_VEHICLE:
        if (evt < HookMgr::VEHICLE_EVENT_COUNT)
        {
            VehicleEventBindings->Insert(evt, functionRef);
            return;
        }
        break;

    case HookMgr::REGTYPE_PACKET:
        if (evt < HookMgr::PACKET_EVENT_COUNT)
        {
            if (id >= NUM_MSG_TYPES)
            {
                luaL_unref(L, LUA_REGISTRYINDEX, functionRef);
                luaL_error(L, "Couldn't find a creature with (ID: %d)!", id);
                return;
            }

            PacketEventBindings->Insert(id, evt, functionRef);
            return;
        }
        break;

    case HookMgr::REGTYPE_CREATURE:
        if (evt < HookMgr::CREATURE_EVENT_COUNT)
        {
            if (!eObjectMgr->GetCreatureTemplate(id))
            {
                luaL_unref(L, LUA_REGISTRYINDEX, functionRef);
                luaL_error(L, "Couldn't find a creature with (ID: %d)!", id);
                return;
            }

            CreatureEventBindings->Insert(id, evt, functionRef);
            return;
        }
        break;

    case HookMgr::REGTYPE_CREATURE_GOSSIP:
        if (evt < HookMgr::GOSSIP_EVENT_COUNT)
        {
            if (!eObjectMgr->GetCreatureTemplate(id))
            {
                luaL_unref(L, LUA_REGISTRYINDEX, functionRef);
                luaL_error(L, "Couldn't find a creature with (ID: %d)!", id);
                return;
            }

            CreatureGossipBindings->Insert(id, evt, functionRef);
            return;
        }
        break;

    case HookMgr::REGTYPE_GAMEOBJECT:
        if (evt < HookMgr::GAMEOBJECT_EVENT_COUNT)
        {
            if (!eObjectMgr->GetGameObjectTemplate(id))
            {
                luaL_unref(L, LUA_REGISTRYINDEX, functionRef);
                luaL_error(L, "Couldn't find a gameobject with (ID: %d)!", id);
                return;
            }

            GameObjectEventBindings->Insert(id, evt, functionRef);
            return;
        }
        break;

    case HookMgr::REGTYPE_GAMEOBJECT_GOSSIP:
        if (evt < HookMgr::GOSSIP_EVENT_COUNT)
        {
            if (!eObjectMgr->GetGameObjectTemplate(id))
            {
                luaL_unref(L, LUA_REGISTRYINDEX, functionRef);
                luaL_error(L, "Couldn't find a gameobject with (ID: %d)!", id);
                return;
            }

            GameObjectGossipBindings->Insert(id, evt, functionRef);
            return;
        }
        break;

    case HookMgr::REGTYPE_ITEM:
        if (evt < HookMgr::ITEM_EVENT_COUNT)
        {
            if (!eObjectMgr->GetItemTemplate(id))
            {
                luaL_unref(L, LUA_REGISTRYINDEX, functionRef);
                luaL_error(L, "Couldn't find a item with (ID: %d)!", id);
                return;
            }

            ItemEventBindings->Insert(id, evt, functionRef);
            return;
        }
        break;

    case HookMgr::REGTYPE_ITEM_GOSSIP:
        if (evt < HookMgr::GOSSIP_EVENT_COUNT)
        {
            if (!eObjectMgr->GetItemTemplate(id))
            {
                luaL_unref(L, LUA_REGISTRYINDEX, functionRef);
                luaL_error(L, "Couldn't find a item with (ID: %d)!", id);
                return;
            }

            ItemGossipBindings->Insert(id, evt, functionRef);
            return;
        }
        break;

    case HookMgr::REGTYPE_PLAYER_GOSSIP:
        if (evt < HookMgr::GOSSIP_EVENT_COUNT)
        {
            playerGossipBindings->Insert(id, evt, functionRef);
            return;
        }
        break;
    }
    luaL_unref(L, LUA_REGISTRYINDEX, functionRef);
    luaL_error(L, "Unknown event type (regtype %d, id %d, event %d)", regtype, id, evt);
}

EventMgr::LuaEvent::LuaEvent(Eluna& _E, EventProcessor* _events, int _funcRef, uint32 _delay, uint32 _calls, Object* _obj):
E(_E), events(_events), funcRef(_funcRef), delay(_delay), calls(_calls), obj(_obj)
{
    if (_events)
        E.m_EventMgr->LuaEvents[_events].insert(this); // Able to access the event if we have the processor
}

EventMgr::LuaEvent::~LuaEvent()
{
    if (events)
    {
        // Attempt to remove the pointer from LuaEvents
        EventMgr::EventMap::const_iterator it = E.m_EventMgr->LuaEvents.find(events); // Get event set
        if (it != E.m_EventMgr->LuaEvents.end())
            E.m_EventMgr->LuaEvents[events].erase(this);// Remove pointer
    }
    luaL_unref(E.L, LUA_REGISTRYINDEX, funcRef); // Free lua function ref
}

bool EventMgr::LuaEvent::Execute(uint64 /*time*/, uint32 /*diff*/)
{
    bool remove = (calls == 1);
    if (!remove)
        events->AddEvent(this, events->CalculateTime(delay)); // Reschedule before calling incase RemoveEvents used
    lua_rawgeti(E.L, LUA_REGISTRYINDEX, funcRef);
    Eluna::Push(E.L, funcRef);
    Eluna::Push(E.L, delay);
    Eluna::Push(E.L, calls);
    if (!remove && calls)
        --calls;
    Eluna::Push(E.L, obj);
    Eluna::ExecuteCall(E.L, 4, 0);
    return remove; // Destory (true) event if not run
}
