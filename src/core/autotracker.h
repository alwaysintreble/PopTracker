#ifndef _CORE_AUTOTRACKER_H
#define _CORE_AUTOTRACKER_H

#include "../usb2snes/usb2snes.h"
#include "../uat/uatclient.h"
#include "../ap/aptracker.h"
#include "signal.h"
#include <string>
#include <string.h>
#include <stdint.h>
#include <vector> // TODO: replace by uint8_t* ?
#include <map>
#include <list>
#include <set>
#include <chrono>
#include <thread>
#include "../luaglue/luainterface.h"
#include "../luaglue/lua_json.h"
#include "util.h"


// this class is designed to wrap multiple auto-tracker back-ends
class AutoTracker final : public LuaInterface<AutoTracker>{
    friend class LuaInterface;
    
public:
    AutoTracker(const std::string& platform, const std::set<std::string>& flags, const std::string& name="PopTracker")
            : _name(name)
    {
        if (flags.find("ap") != flags.end()) {
            _ap = new APTracker(_name);
            _lastBackendIndex++;
            int apIndex = _lastBackendIndex;
            _backendIndex[_ap] = apIndex;
            _state.push_back(State::Disabled);
            _ap->onError += {this, [this, apIndex](void* sender, const std::string& msg) {
                if (_state[apIndex] != State::Disabled && _state[apIndex] != State::Disconnected) {
                    _state[apIndex] = State::Disconnected;
                    onStateChange.emit(this, apIndex, _state[apIndex]);
                }
                if (!msg.empty()) {
                    onError.emit(this, "AP: " + msg);
                }
            }};
            _ap->onStateChanged += {this, [this, apIndex](void* sender, auto state) {
                State newstate =
                        state == APClient::State::SLOT_CONNECTED ? State::ConsoleConnected :
                        state == APClient::State::ROOM_INFO ? State::BridgeConnected :
                        State::Disconnected;
                if (_state[apIndex] != newstate) {
                    _state[apIndex] = newstate;
                    onStateChange.emit(this, apIndex, _state[apIndex]);
                }
            }};
        }
        if (strcasecmp(platform.c_str(), "snes")==0) {
            _snes = new USB2SNES(_name);
            _lastBackendIndex++;
            _backendIndex[_snes] = _lastBackendIndex;
            _state.push_back(State::Disconnected);
            if (flags.find("lorom") != flags.end()) {
                _snes->setMapping(USB2SNES::Mapping::LOROM);
            } else if (flags.find("hirom") != flags.end()) {
                _snes->setMapping(USB2SNES::Mapping::HIROM);
            } else if (flags.find("exlorom") != flags.end()) {
                _snes->setMapping(USB2SNES::Mapping::EXLOROM);
            } else if (flags.find("exhirom") != flags.end()) {
                _snes->setMapping(USB2SNES::Mapping::EXHIROM);
            }
        }
        if (flags.find("uat") != flags.end()) {
            _uat = new UATClient();
            _lastBackendIndex++;
            _backendIndex[_uat] = _lastBackendIndex;
            _state.push_back(State::Disconnected);
            _uat->setInfoHandler([this](const UATClient::Info& info) {
                // TODO: let user select the slot and send sync after that
                _slot = info.slots.empty() ? "" : info.slots.front();
                if (!_slot.empty()) printf("slot selected: %s\n", sanitize_print(_slot).c_str());
                _uat->sync(_slot);
            });
            _uat->setVarHandler([this](const std::vector<UATClient::Var>& vars) {
                std::list<std::string> varNames;
                for (const auto& var : vars) {
                    if (var.slot != _slot) continue;
                    printf("%s:%s = %s\n",
                            sanitize_print(var.slot).c_str(),
                            sanitize_print(var.name).c_str(),
                            var.value.dump().c_str());
                    _vars[var.name] = var.value;
                    varNames.push_back(var.name);
                }
                // NOTE: for UAT we pass the event straight through
                onVariablesChanged.emit(this, varNames);
            });
        }
        if (_state.empty()) {
            printf("No auto-tracking back-end for %s%s\n",
                    platform.c_str(),
                    flags.empty() ? " (no flags)" : "");
        }
    }
    
    virtual ~AutoTracker()
    {
        bool spawnedWorkers = false;
        
        if (_snes) {
            if (_snes->mayBlockOnExit()) {
                // delete() may wait for socket timeout -> run destructor in another thread
                auto snes = _snes;
                std::thread([snes]() { delete snes; }).detach();
                spawnedWorkers = true;
            } else {
                delete _snes;
            }
        }
        _snes = nullptr;
        
        if (_uat) delete _uat;
        _uat = nullptr;

        if (_ap) delete _ap;
        _ap = nullptr;

        if (spawnedWorkers) {
            // wait a bit if we started a thread to increase readability of logs
            std::this_thread::sleep_for(std::chrono::milliseconds(21));
        }
    }
    
    enum class State {
        Unavailable = -1,
        Disabled = 0,
        Disconnected, 
        BridgeConnected,
        ConsoleConnected
    };

    Signal<int, State> onStateChange;
    Signal<> onDataChange;
    Signal<const std::list<std::string>&> onVariablesChanged;
    Signal<const std::string&> onError;
    State getState(int index) const { return _state.size() < (size_t)index ? State::Unavailable : _state[index]; }
    State getState(const std::string& name) {
        if (name == BACKEND_AP_NAME)  return _ap ? getState(_backendIndex[_ap]) : State::Unavailable;
        if (name == BACKEND_UAT_NAME) return _uat ? getState(_backendIndex[_uat]) : State::Unavailable;
        if (name == BACKEND_SNES_NAME)  return _snes ? getState(_backendIndex[_snes]) : State::Unavailable;
        return State::Unavailable;
    }

    const std::string& getName(int index)
    {
        if (_ap && _backendIndex[_ap] == index) return BACKEND_AP_NAME;
        if (_uat && _backendIndex[_uat] == index) return BACKEND_UAT_NAME;
        if (_snes && _backendIndex[_snes] == index) return BACKEND_SNES_NAME;
        return BACKEND_NONE_NAME;
    }

    std::string getSubName(int index)
    {
        if (_snes && _backendIndex[_snes] == index) return _snes->getDeviceName();
        return "";
    }

    bool cycle(int index)
    {
        if (_snes && _backendIndex[_snes] == index) {
            if (getState(index) == AutoTracker::State::Disabled) {
                enable(index);
            } else {
                _snes->nextDevice();
                if (_state[index] > State::BridgeConnected) {
                    _state[index] = State::BridgeConnected;
                }
                onStateChange.emit(this, index, _state[index]);
            }
            return true;
        }
        return false;
    }

    bool doStuff()
    {
        bool res = false;

        if (!_sentState) {
            _sentState = true;
            for (int i=0; i<(int)_state.size(); i++)
                onStateChange.emit(this, i, _state[i]);
        }

        // USB2SNES AUTOTRACKING
        if (_snes && backendEnabled(_snes)) {
            if (_snesAddresses.empty())
                _snes->connect();
            else
                _snes->connect(_snesAddresses);
            if (_snes->dostuff()) {
                int index = _backendIndex[_snes];
                State oldState = _state[index];
                bool wsConnected = _snes->wsConnected();
                bool snesConnected = wsConnected ? _snes->snesConnected() : false;

                if (snesConnected) {
                    _state[index] = State::ConsoleConnected;
                } else if (wsConnected) {
                    _state[index] = State::BridgeConnected;
                } else {
                    _state[index] = State::Disconnected;
                }

                if (_state[index] != oldState) {
                    onStateChange.emit(this, index, _state[index]);
                } else if (_state[index] == State::ConsoleConnected) {
                    onDataChange.emit(this);
                }

                res = true;
            }
        }

        // UAT AUTOTRACKING
        if (_uat && backendEnabled(_uat)) {
            _uat->connect();
            if (_uat->poll()) {
                int index = _backendIndex[_uat];
                State oldState = _state[index];
                _state[index] =
                        _uat->getState() == UATClient::State::GAME_CONNECTED ? State::ConsoleConnected :
                        _uat->getState() == UATClient::State::SOCKET_CONNECTED ? State::BridgeConnected :
                        State::Disconnected;
                if (_state[index] != oldState) {
                    onStateChange.emit(this, index, _state[index]);
                }
                res = true;
            }
        }

        // AP AUTOTRACKING
        if (_ap && backendEnabled(_ap)) {
            if (_ap->poll())
                res = true;
        }

        return res;
    }
    
    bool addWatch(unsigned addr, unsigned len)
    {
        if (len<0) return false;
        if (addr<=0xffffff && _snes) {
            _snes->addWatch((uint32_t)addr, len);
            return true;
        }
        return false;
    }

    bool removeWatch(unsigned addr, unsigned len)
    {
        if (len<0) return false;
        if (addr<=0xffffff && _snes) {
            _snes->removeWatch((uint32_t)addr, len);
            return true;
        }
        return false;
    }

    void setInterval(unsigned ms) {
        if (_snes)
            _snes->setUpdateInterval(ms);
    }

    void clearCache() {
        if (_snes)
            _snes->clearCache();
        if (_uat)
            _uat->sync(_slot);
    }
    
    // TODO: canRead(addr,len) to detect incomplete segment
    std::vector<uint8_t> read(unsigned addr, unsigned len)
    {
        std::vector<uint8_t> res;
        if (_snes) {
            uint8_t buf[len];
            _snes->read((uint32_t)addr, len, buf);
            for (size_t i=0; i<len; i++)
                res.push_back(buf[i]);
        }
        return res;
    }
    
    int ReadU8(int segment, int offset=0)
    {
        // NOTE: this is AutoTracker:Read8. we only have 1 segment, that is AutoTracker
        return ReadUInt8(segment+offset);
    }

    int ReadU16(int segment, int offset=0) { return ReadUInt16(segment+offset); }
    int ReadU24(int segment, int offset=0) { return ReadUInt24(segment+offset); }
    int ReadU32(int segment, int offset=0) { return ReadUInt32(segment+offset); }

    int ReadUInt8(int addr)
    {
        // NOTE: this is Segment:ReadUint8. we only have 1 segment, that is AutoTracker
        if (_snes) {
            auto res = _snes->read(addr);
            if (res == 0) _snes->addWatch(addr); // we don't read snes memory on the main thread.
            // TODO: canRead + maybe wait a little and try again
            //printf("$%06x = %02x\n", a, res);
            return res;
        }
        return 0;
    }

    int ReadUInt16(int addr)
    {
        if (_snes) {
            auto res = _snes->readInt<uint16_t>(addr);
            if (res == 0) _snes->addWatch(addr,2);
            return res;
        }
        return 0;
    }

    int ReadUInt24(int addr)
    {
        if (_snes) {
            auto res = _snes->readInt<uint32_t>(addr)&0xffffff;
            if (res == 0) _snes->addWatch(addr,3);
            return res;
        }
        return 0;
    }

    int ReadUInt32(int addr)
    {
        if (_snes) {
            auto res = _snes->readInt<uint32_t>(addr);
            if (res == 0) _snes->addWatch(addr,4);
            return res;
        }
        return 0;
    }

    json ReadVariable(const std::string& name)
    {
        auto it = _vars.find(name);
        if (it != _vars.end()) {
            return it->second;
        }
        return {};
    }

    int GetConnectionState(const std::string& backend)
    {
        return (int)getState(backend);
    }

    bool enable(int index, const std::string& uri="", const std::string& slot="", const std::string& password="")
    {
        if (_state.size() < (size_t)index) return false;
        if (_state[index] != State::Disabled) return true;
        if ((_snes && _backendIndex[_snes] == index)
                || (_uat && _backendIndex[_uat] == index)) {
            // snes and uat will auto-connect when polling (doStuff())
            _state[index] = State::Disconnected;
            onStateChange.emit(this, index, _state[index]);
            return true;
        }
        else if (_ap && _backendIndex[_ap] == index) {
            // ap requires explicit connection to a server
            if (_ap->connect(uri, slot, password)) {
                _state[index] = State::Disconnected;
                onStateChange.emit(this, index, _state[index]);
                return true;
            }
        }
        return false;
    }

    void disable(int index)
    {
        // -1 = all
        if (_state.empty()) return;
        int tmp = index;
        for (int index = ((tmp==-1)?0:tmp); index < ((tmp==-1)?(int)_state.size():(tmp+1)); index++) {
            if (_state.size() < (size_t)index) return;
            if (_state[index] == State::Unavailable) continue;
            _state[index] = State::Disabled;
            if (_snes && _backendIndex[_snes] == index) {
                // connect() after disconnect() needs to join the worker thread,
                // which may block. as a work-around we start a new USB2SNES and
                // destruct the old one on a different thread.
                // We should probably rewrite USB2SNES support.
                if (_snes->mayBlockOnExit()) {
                    auto snes = _snes;
                    std::thread([snes]() { delete snes; }).detach();
                } else {
                    delete _snes;
                }
                _snes = new USB2SNES(_name);
                _backendIndex.erase(_snes);
                _backendIndex[_snes] = index;
            }
            if (_uat && _backendIndex[_uat] == index
                    && _uat->getState() != UATClient::State::DISCONNECTED
                    && _uat->getState() != UATClient::State::DISCONNECTING) {
                _uat->disconnect(websocketpp::close::status::going_away);
            }
            if (_ap && _backendIndex[_ap] == index)
                _ap->disconnect();
            onStateChange.emit(this, index, _state[index]);
        }
    }

    APTracker* getAP() const
    {
        return _ap;
    }

    void setSnesAddresses(const std::vector<std::string>& addresses)
    {
        _snesAddresses = addresses;
        // fix up addresses:
        // 1. add ws:// if missing
        // 2. add both legacy and new ports if missing
        auto count = _snesAddresses.size();
        for (size_t i=0; i<count; i++) {
            auto& addr = _snesAddresses[i];
            auto p = addr.find("://");
            if (addr.length()<3 || p == addr.npos) {
                addr.insert(0, "ws://");
                p = 2;
            }
            if (addr.find(":", p+3) == addr.npos) {
                std::string legacy = addr + ":8080"; // legacy port
                addr += ":23074"; // qusb port
                _snesAddresses.push_back(legacy); // this invalidates addr ref
            }
        }
    }

protected:
    std::map<void*, int> _backendIndex;
    int _lastBackendIndex = -1;
    std::vector<State> _state;
    USB2SNES *_snes = nullptr;
    UATClient *_uat = nullptr;
    APTracker *_ap = nullptr;
    std::string _slot; // selected slot for UAT
    std::map<std::string, nlohmann::json> _vars; // variable store for UAT
    std::string _name;
    bool _sentState = false;
    std::vector<std::string> _snesAddresses;

    static const std::string BACKEND_AP_NAME;
    static const std::string BACKEND_UAT_NAME;
    static const std::string BACKEND_SNES_NAME;
    static const std::string BACKEND_NONE_NAME;

    bool backendEnabled(void* backend)
    {
        if (!backend) return false;
        int index = _backendIndex[backend];
        return (_state[index] != State::Disabled && _state[index] != State::Unavailable);
    }

protected: // Lua interface implementation
    static constexpr const char Lua_Name[] = "AutoTracker";
    static const LuaInterface::MethodMap Lua_Methods;

    virtual int Lua_Index(lua_State *L, const char* key) override;
};

#endif //_CORE_AUTOTRACKER_H
