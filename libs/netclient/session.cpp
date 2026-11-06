#include "session.h"

#include "netcommon/netcommon.h"

#include <enet/enet.h>

#include <algorithm>
#include <cerrno>
#include <utility>

namespace gage::net {

    namespace {
        // A stalled owner (a loading screen) must not pile up seconds of world frames it
        // will never use: past this many unread World (or Coarse) events the two oldest are
        // merged into one. Merged, not dropped: frames are partial, so a dropped one could
        // be the only sighting of an idle player.
        constexpr size_t kMaxPendingWorld = 4;

        uint8_t ChannelFor(proto::Msg t)
        {
            return t == proto::Msg::WorldState || t == proto::Msg::WorldCoarse || t == proto::Msg::VehicleWorld ||
                           t == proto::Msg::PlayerState || t == proto::Msg::VehicleUpdate || t == proto::Msg::LinkInfo   // R3 T5: LinkInfo rides the state channel
                       ? proto::kChanState
                       : proto::kChanReliable;
        }

        // `newer` absorbs `older`: its entries win per id; older entries of ids that
        // `newer` says left are dropped; the leave list is the union minus the ids that
        // `newer` carries again. Both entry lists are ascending by id (the decoder
        // guarantees it), and so is the result.
        void MergeWorld(proto::WorldState& newer, proto::WorldState&& older)
        {
            std::vector<uint16_t> leftNewer = newer.left;
            std::sort(leftNewer.begin(), leftNewer.end());
            auto leftIn = [&](uint16_t id) { return std::binary_search(leftNewer.begin(), leftNewer.end(), id); };
            std::vector<proto::WorldEntry> merged;
            merged.reserve(older.players.size() + newer.players.size());
            size_t i = 0, j = 0;
            while (i < older.players.size() || j < newer.players.size())
            {
                if (j >= newer.players.size() || (i < older.players.size() && older.players[i].id < newer.players[j].id))
                {
                    if (!leftIn(older.players[i].id)) merged.push_back(older.players[i]);
                    i++;
                }
                else
                {
                    if (i < older.players.size() && older.players[i].id == newer.players[j].id) i++;
                    merged.push_back(newer.players[j]);
                    j++;
                }
            }
            std::vector<uint16_t> left = std::move(older.left);
            left.insert(left.end(), newer.left.begin(), newer.left.end());
            std::sort(left.begin(), left.end());
            left.erase(std::unique(left.begin(), left.end()), left.end());
            std::vector<uint16_t> idsNewer;
            idsNewer.reserve(newer.players.size());
            for (const proto::WorldEntry& e : newer.players) idsNewer.push_back(e.id);
            left.erase(std::remove_if(left.begin(), left.end(),
                                      [&](uint16_t id) { return std::binary_search(idsNewer.begin(), idsNewer.end(), id); }),
                       left.end());
            newer.players = std::move(merged);
            newer.left = std::move(left);
        }

        void MergeCoarse(proto::CoarseState& newer, proto::CoarseState&& older)
        {
            std::vector<proto::CoarseEntry> merged;
            merged.reserve(older.players.size() + newer.players.size());
            size_t i = 0, j = 0;
            while (i < older.players.size() || j < newer.players.size())
            {
                if (j >= newer.players.size() || (i < older.players.size() && older.players[i].id < newer.players[j].id))
                    merged.push_back(older.players[i++]);
                else
                {
                    if (i < older.players.size() && older.players[i].id == newer.players[j].id) i++;
                    merged.push_back(newer.players[j++]);
                }
            }
            newer.players = std::move(merged);
        }

        // v5: a newer motion-only entry (only the motion and the driver flags) laid over the older entry of the same
        // vehicle. Over an older FULL one it makes a full state again (the statics of the older, the motion, flags
        // and age of the newer: nothing is lost by merging); over an older motion-only one, or with nothing
        // older, it stays what it is. A full newer entry replaces the older one.
        proto::VehicleState OverlayVehicle(const proto::VehicleState& older, const proto::VehicleState& newer)
        {
            if (!newer.motionOnly || older.motionOnly) return newer;
            proto::VehicleState s = older;
            s.k = newer.k;
            s.rot = newer.rot;
            s.avx = newer.avx;
            s.avy = newer.avy;
            s.avz = newer.avz;
            s.steer = newer.steer;
            s.flags = newer.flags;
            s.ageMs = newer.ageMs;
            return s;
        }

        // `newer` absorbs `older` (both VehicleWorld): the newer `present` set stands (it is complete);
        // a full state of the older frame survives for an id the newer one still lists but did not
        // refresh (and is overlaid by a newer motion-only entry of that id); `removed` is the union minus
        // ids the newer frame lists as present again.
        void MergeVehicles(proto::VehicleWorld& newer, proto::VehicleWorld&& older)
        {
            std::vector<proto::VehicleState> merged;
            merged.reserve(newer.vehicles.size() + older.vehicles.size());
            size_t i = 0, j = 0;
            auto presentNow = [&](uint16_t id) { return std::binary_search(newer.present.begin(), newer.present.end(), id); };
            while (i < older.vehicles.size() || j < newer.vehicles.size())
            {
                if (j >= newer.vehicles.size() || (i < older.vehicles.size() && older.vehicles[i].id < newer.vehicles[j].id))
                {
                    if (presentNow(older.vehicles[i].id)) merged.push_back(older.vehicles[i]);
                    i++;
                }
                else
                {
                    if (i < older.vehicles.size() && older.vehicles[i].id == newer.vehicles[j].id)
                    {
                        merged.push_back(OverlayVehicle(older.vehicles[i], newer.vehicles[j]));
                        i++;
                        j++;
                    }
                    else
                        merged.push_back(newer.vehicles[j++]);
                }
            }
            std::vector<uint16_t> removed = std::move(older.removed);
            removed.insert(removed.end(), newer.removed.begin(), newer.removed.end());
            std::sort(removed.begin(), removed.end());
            removed.erase(std::unique(removed.begin(), removed.end()), removed.end());
            removed.erase(std::remove_if(removed.begin(), removed.end(), presentNow), removed.end());
            newer.vehicles = std::move(merged);
            newer.removed = std::move(removed);
        }
    }

    struct ClientSession::Impl
    {
        DropSim dropWorld;
    };

    ClientSession::ClientSession() : impl_(new Impl) {}

    ClientSession::~ClientSession()
    {
        // Tell the server we are leaving if we are still connected: one immediate
        // send, no waiting (the server falls back to a timeout if it is lost).
        if (peer_ && (state_ == SessionState::Joining || state_ == SessionState::Joined ||
                      state_ == SessionState::Closing))
            enet_peer_disconnect_now(peer_, proto::kDisconnectQuit);
        if (host_)
            enet_host_destroy(host_);
    }

    bool ClientSession::Connect(const SessionOptions& opts, std::string* err)
    {
        auto fail = [&](const char* why) {
            if (err) *err = why;
            return false;
        };
        if (state_ == SessionState::Connecting || state_ == SessionState::Joining ||
            state_ == SessionState::Joined || state_ == SessionState::Closing)
            return fail("already connected");

        if (host_)
        {
            enet_host_destroy(host_);
            host_ = nullptr;
            peer_ = nullptr;
        }
        events_.clear();
        refused_ = false;
        myId_ = 0;
        tickHz_ = 0;
        serverName_.clear();
        seq_ = 0;
        vehSeq_ = 0;
        lookSent_ = false; // a new connection: the server knows no look of ours
        worldReceived_ = worldDropped_ = malformed_ = 0;
        decoder_.Reset();
        counters_ = Counters();
        lastWireIn_ = lastWireInPackets_ = lastWireOut_ = lastWireOutPackets_ = 0; // a new ENet host counts from 0
        opts_ = opts;
        impl_->dropWorld.Configure(opts.dropWorldPercent, opts.dropSeed);

        enet_ = std::make_unique<EnetScope>();
        if (!enet_->ok()) return fail("ENet failed to initialise");

        ENetAddress addr;
        addr.port = opts.port;
        if (enet_address_set_host_ip(&addr, opts.host.c_str()) != 0 &&
            enet_address_set_host(&addr, opts.host.c_str()) != 0)
            return fail("cannot resolve host");

        host_ = enet_host_create(nullptr, 1, proto::kChannelCount, 0, 0);
        if (!host_) return fail("cannot create the ENet host");
        peer_ = enet_host_connect(host_, &addr, proto::kChannelCount, 0);
        if (!peer_)
        {
            enet_host_destroy(host_);
            host_ = nullptr;
            return fail("cannot start the ENet connection");
        }
        enet_peer_timeout(peer_, opts.peerTimeoutLimit, opts.peerTimeoutMinMs, opts.peerTimeoutMaxMs);
        state_ = SessionState::Connecting;
        deadlineMs_ = NowMs() + opts.connectTimeoutMs;
        return true;
    }

    void ClientSession::Poll(uint32_t timeoutMs)
    {
        if (!host_) return;
        ENetEvent ev;
        // A failed service call means the socket itself errored; ENet then just stops
        // talking and the peer times out, so count it for whoever is diagnosing that.
        auto noteFailure = [this](int r) {
            if (r >= 0) return;
            ++counters_.socketErrors;
#ifdef _WIN32
            counters_.lastSocketError = WSAGetLastError();
#else
            counters_.lastSocketError = errno;
#endif
        };
        int r = enet_host_service(host_, &ev, timeoutMs);
        noteFailure(r);
        while (r > 0 && host_)
        {
            switch (ev.type)
            {
            case ENET_EVENT_TYPE_CONNECT:
                if (state_ == SessionState::Connecting)
                {
                    state_ = SessionState::Joining;
                    if (opts_.autoHello)
                    {
                        proto::Hello h;
                        h.version = opts_.protocolVersion;
                        h.name = opts_.name;
                        SendBytes(proto::kChanReliable, proto::Encode(h), true);
                    }
                }
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                HandleReceive(ev.channelID, ev.packet->data, ev.packet->dataLength);
                enet_packet_destroy(ev.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                HandleDisconnect(ev.data);
                break;
            case ENET_EVENT_TYPE_NONE:
                break;
            }
            if (host_)
            {
                r = enet_host_service(host_, &ev, 0);
                noteFailure(r);
            }
        }
        if (!host_) return;
        SampleWire();

        const uint64_t now = NowMs();
        if ((state_ == SessionState::Connecting || state_ == SessionState::Joining) && now >= deadlineMs_)
            Finish(state_ == SessionState::Connecting ? DisconnectReason::ConnectFailed
                                                      : DisconnectReason::HandshakeTimeout, true);
        else if (state_ == SessionState::Closing && now >= deadlineMs_)
            Finish(DisconnectReason::Local, true);
    }

    bool ClientSession::NextEvent(Event& out)
    {
        if (events_.empty()) return false;
        out = std::move(events_.front());
        events_.pop_front();
        return true;
    }

    void ClientSession::Push(Event&& e)
    {
        if (e.type == Event::Type::World || e.type == Event::Type::Coarse || e.type == Event::Type::VehicleWorld)
        {
            const Event::Type t = e.type;
            size_t pending = 0;
            for (const Event& q : events_)
                if (q.type == t) pending++;
            if (pending >= kMaxPendingWorld)
            {
                // Fold the oldest unread event of this kind into the next unread one of the same kind.
                auto first = events_.end();
                for (auto it = events_.begin(); it != events_.end(); ++it)
                {
                    if (it->type != t) continue;
                    if (first == events_.end())
                    {
                        first = it;
                        continue;
                    }
                    if (t == Event::Type::World)
                        MergeWorld(it->world, std::move(first->world));
                    else if (t == Event::Type::Coarse)
                        MergeCoarse(it->coarse, std::move(first->coarse));
                    else
                        MergeVehicles(it->vehicles, std::move(first->vehicles));
                    events_.erase(first);
                    break;
                }
            }
        }
        events_.push_back(std::move(e));
    }

    void ClientSession::HandleReceive(uint8_t channel, const uint8_t* data, size_t len)
    {
        proto::Msg type;
        if (!proto::PeekType(data, len, type) || channel != ChannelFor(type))
        {
            malformed_++;
            return;
        }
        const bool joined = state_ == SessionState::Joined;
        Event e;
        switch (type)
        {
        case proto::Msg::Welcome:
            if (state_ != SessionState::Joining) return; // a repeat or out-of-order Welcome
            if (!proto::Decode(data, len, e.welcome)) { malformed_++; return; }
            state_ = SessionState::Joined;
            myId_ = e.welcome.yourId;
            tickHz_ = e.welcome.tickHz;
            serverName_ = e.welcome.serverName;
            playerHealth_ = e.welcome.playerHealth;
            e.type = Event::Type::Welcome;
            break;
        case proto::Msg::Refuse:
            if (!proto::Decode(data, len, e.refuse)) { malformed_++; return; }
            refused_ = true; // the server disconnects right after
            e.type = Event::Type::Refused;
            break;
        // Everything below only means something once Welcome has set the stage;
        // reliable ordering puts Welcome first, so an earlier one is stray.
        case proto::Msg::PlayerJoined:
            if (!joined) return;
            if (!proto::Decode(data, len, e.joined)) { malformed_++; return; }
            e.type = Event::Type::PlayerJoined;
            break;
        case proto::Msg::PlayerLeft:
            if (!joined) return;
            if (!proto::Decode(data, len, e.left)) { malformed_++; return; }
            e.type = Event::Type::PlayerLeft;
            break;
        case proto::Msg::ChatMessage:
            if (!joined) return;
            if (!proto::Decode(data, len, e.chat)) { malformed_++; return; }
            e.type = Event::Type::Chat;
            break;
        case proto::Msg::WorldState:
        {
            if (!joined) return;
            worldReceived_++;
            // The loss knob acts BEFORE decoding: a frame that "never arrived" must not be
            // decoded, or it would enter the delta history and be acknowledged to the server.
            if (impl_->dropWorld.Drop()) { worldDropped_++; return; }
            const uint64_t staleBefore = decoder_.framesStale();
            if (!decoder_.Decode(data, len, e.world))
            {
                if (decoder_.framesStale() == staleBefore) malformed_++; // a late duplicate is not an error
                return;
            }
            counters_.worldPackets++;
            counters_.worldBytes += len;
            counters_.worldEntries += e.world.players.size();
            e.type = Event::Type::World;
            break;
        }
        case proto::Msg::WorldCoarse:
            if (!joined) return;
            if (!proto::Decode(data, len, e.coarse)) { malformed_++; return; }
            counters_.coarsePackets++;
            counters_.coarseBytes += len;
            counters_.coarseEntries += e.coarse.players.size();
            e.type = Event::Type::Coarse;
            break;
        case proto::Msg::PeerLook:
            if (!joined) return;
            if (!proto::Decode(data, len, e.look) || !proto::ValidLook(e.look.look)) { malformed_++; return; }
            e.type = Event::Type::PeerLook;
            break;
        case proto::Msg::VehicleWorld:
            if (!joined) return;
            if (!proto::Decode(data, len, e.vehicles)) { malformed_++; return; }
            e.type = Event::Type::VehicleWorld;
            break;
        case proto::Msg::VehicleResult:
            if (!joined) return;
            if (!proto::Decode(data, len, e.vehicleResult)) { malformed_++; return; }
            e.type = Event::Type::VehicleResult;
            break;
        case proto::Msg::Damage:
            if (!joined) return;
            if (!proto::Decode(data, len, e.damage)) { malformed_++; return; }
            e.type = Event::Type::Damage;
            break;
        case proto::Msg::Killed:
            if (!joined) return;
            if (!proto::Decode(data, len, e.killed)) { malformed_++; return; }
            e.type = Event::Type::Killed;
            break;
        case proto::Msg::RespawnOk:
            if (!joined) return;
            if (!proto::Decode(data, len, e.respawn)) { malformed_++; return; }
            e.type = Event::Type::RespawnOk;
            break;
        case proto::Msg::PeerAnim:
            if (!joined) return;
            if (!proto::Decode(data, len, e.anim) || !proto::ValidAnim(e.anim.anim)) { malformed_++; return; }
            e.type = Event::Type::PeerAnim;
            break;
        case proto::Msg::PeerScenario:
            if (!joined) return;
            if (!proto::Decode(data, len, e.scenario) || !proto::ValidAnimName(e.scenario.name)) { malformed_++; return; }
            e.type = Event::Type::PeerScenario;
            break;
        case proto::Msg::PeerAnimStop:
            if (!joined) return;
            if (!proto::Decode(data, len, e.animStop)) { malformed_++; return; }
            e.type = Event::Type::PeerAnimStop;
            break;
        case proto::Msg::SeatNotice:
            if (!joined) return;
            if (!proto::Decode(data, len, e.seatNotice)) { malformed_++; return; }
            e.type = Event::Type::SeatNotice;
            break;
        case proto::Msg::VehicleImpact:
            if (!joined) return;
            if (!proto::Decode(data, len, e.impact)) { malformed_++; return; }
            e.type = Event::Type::VehicleImpact;
            break;
        case proto::Msg::LinkInfo:
            if (!joined) return;
            if (!proto::Decode(data, len, e.linkInfo)) { malformed_++; return; }
            e.type = Event::Type::LinkInfo;
            break;
        case proto::Msg::ResourceManifest:
        case proto::Msg::ResourceChunk:
            return; // server-to-client types this session does not surface yet: ignored, not an error
        case proto::Msg::Hello:
        case proto::Msg::PlayerState:
        case proto::Msg::ChatSend:
        case proto::Msg::ResourceRequest:
        case proto::Msg::VehicleCmd:
        case proto::Msg::PlayerLook:
        case proto::Msg::VehicleUpdate:
        case proto::Msg::Hit:
        case proto::Msg::Respawn:
        case proto::Msg::PlayAnim:
        case proto::Msg::PlayScenario:
        case proto::Msg::StopAnim:
            malformed_++; // client-to-server types never arrive here
            return;
        }
        // PeekType accepts the whole numeric range, so a type in a reserved gap matches no case above and would
        // reach Push as the default Event (Disconnected): the engine would end a live session. Nothing in this
        // switch surfaces a Disconnected (only HandleDisconnect does), and the switch stays default-free so
        // -Wswitch still flags a new Msg value.
        if (e.type == Event::Type::Disconnected) { malformed_++; return; }
        Push(std::move(e));
    }

    void ClientSession::HandleDisconnect(uint32_t data)
    {
        DisconnectReason why;
        if (state_ == SessionState::Closing) why = DisconnectReason::Local;
        else if (state_ == SessionState::Connecting) why = DisconnectReason::ConnectFailed;
        else if (data == proto::kDisconnectKicked) why = DisconnectReason::Kicked;
        else if (data == proto::kDisconnectShutdown) why = DisconnectReason::Shutdown;
        else if (data == proto::kDisconnectRefused || refused_) why = DisconnectReason::Refused;
        else why = DisconnectReason::Lost;
        Finish(why, true);
    }

    void ClientSession::SampleWire()
    {
        if (!host_) return;
        counters_.wireBytesIn += (uint32_t)(host_->totalReceivedData - lastWireIn_);
        counters_.wireDatagramsIn += (uint32_t)(host_->totalReceivedPackets - lastWireInPackets_);
        counters_.wireBytesOut += (uint32_t)(host_->totalSentData - lastWireOut_);
        counters_.wireDatagramsOut += (uint32_t)(host_->totalSentPackets - lastWireOutPackets_);
        lastWireIn_ = host_->totalReceivedData;
        lastWireInPackets_ = host_->totalReceivedPackets;
        lastWireOut_ = host_->totalSentData;
        lastWireOutPackets_ = host_->totalSentPackets;
    }

    ClientSession::Counters ClientSession::counters() const
    {
        Counters c = counters_;
        c.worldDeltaEntries = decoder_.deltaEntries();
        c.worldAbsoluteEntries = decoder_.absoluteEntries();
        c.worldRejected = decoder_.framesRejected();
        c.worldStale = decoder_.framesStale();
        return c;
    }

    void ClientSession::Finish(DisconnectReason why, bool queueEvent)
    {
        SampleWire();
        if (host_)
        {
            enet_host_destroy(host_);
            host_ = nullptr;
            peer_ = nullptr;
        }
        state_ = SessionState::Closed;
        if (queueEvent)
        {
            Event e;
            e.type = Event::Type::Disconnected;
            e.disconnect = why;
            Push(std::move(e));
        }
    }

    bool ClientSession::SendBytes(uint8_t channel, const proto::Bytes& bytes, bool reliable)
    {
        if (!peer_ || bytes.empty()) return false;
        if (state_ != SessionState::Joining && state_ != SessionState::Joined) return false;
        // Large unreliable packets would otherwise be fragmented RELIABLY, which
        // turns a state channel into a head-of-line-blocked one.
        const enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT;
        ENetPacket* pkt = enet_packet_create(bytes.data(), bytes.size(), flags);
        if (!pkt) return false;
        if (enet_peer_send(peer_, channel, pkt) < 0)
        {
            enet_packet_destroy(pkt); // enet_peer_send only takes ownership on success
            return false;
        }
        return true;
    }

    bool ClientSession::SendState(const proto::Kinematics& k, const proto::PedState& ped, int32_t sampleMs)
    {
        if (state_ != SessionState::Joined || !proto::ValidKinematics(k) || !proto::ValidPedState(ped)) return false;
        proto::PlayerState s;
        s.seq = seq_ + 1;
        s.k = k;
        s.ack = decoder_.ack();
        s.ped = ped;
        s.sampleMs = sampleMs < 0 ? (uint16_t)(NowMs() & 0xFFFFu) : (uint16_t)sampleMs;
        if (!SendBytes(proto::kChanState, proto::Encode(s), false)) return false;
        seq_ = s.seq;
        return true;
    }

    bool ClientSession::SendLook(const proto::Look& look)
    {
        if (state_ != SessionState::Joined || !proto::ValidLook(look)) return false;
        if (lookSent_ && look == lastLook_) return true; // reliable channel: the server has it already
        proto::PlayerLook m;
        m.look = look;
        if (!SendBytes(proto::kChanReliable, proto::Encode(m), true)) return false;
        lookSent_ = true;
        lastLook_ = look;
        return true;
    }

    bool ClientSession::SendChat(const std::string& text)
    {
        if (state_ != SessionState::Joined || !proto::ValidChat(text)) return false;
        proto::ChatSend c;
        c.text = text;
        return SendBytes(proto::kChanReliable, proto::Encode(c), true);
    }

    bool ClientSession::SendVehicleState(const proto::VehicleUpdate& u, int32_t sampleMs)
    {
        if (state_ != SessionState::Joined || u.vehId >= proto::kMaxVehicles || !proto::ValidVehicleUpdate(u)) return false;
        proto::VehicleUpdate m = u;
        m.seq = vehSeq_ + 1;
        m.sampleMs = sampleMs < 0 ? (uint16_t)(NowMs() & 0xFFFFu) : (uint16_t)sampleMs;
        if (!SendBytes(proto::kChanState, proto::Encode(m), false)) return false;
        vehSeq_ = m.seq;
        return true;
    }

    bool ClientSession::SendVehicleCmd(const proto::VehicleCmd& c)
    {
        if (state_ != SessionState::Joined || (uint8_t)c.op < (uint8_t)proto::VehicleOp::Spawn ||
            (uint8_t)c.op > proto::kVehicleOpLast)
            return false;
        return SendBytes(proto::kChanReliable, proto::Encode(c), true);
    }

    bool ClientSession::SendHit(const proto::Hit& h)
    {
        if (state_ != SessionState::Joined || h.damage == 0 ||
            (h.kind != proto::HitKind::Player && h.kind != proto::HitKind::Vehicle))
            return false;
        return SendBytes(proto::kChanReliable, proto::Encode(h), true);
    }

    bool ClientSession::SendRespawn()
    {
        if (state_ != SessionState::Joined) return false;
        return SendBytes(proto::kChanReliable, proto::Encode(proto::Respawn()), true);
    }

    bool ClientSession::SendAnim(const proto::Anim& a)
    {
        if (state_ != SessionState::Joined || !proto::ValidAnim(a)) return false;
        proto::PlayAnim m;
        m.anim = a;
        return SendBytes(proto::kChanReliable, proto::Encode(m), true);
    }

    bool ClientSession::SendScenario(const std::string& name)
    {
        if (state_ != SessionState::Joined || !proto::ValidAnimName(name)) return false;
        proto::PlayScenario m;
        m.name = name;
        return SendBytes(proto::kChanReliable, proto::Encode(m), true);
    }

    bool ClientSession::SendStopAnim()
    {
        if (state_ != SessionState::Joined) return false;
        return SendBytes(proto::kChanReliable, proto::Encode(proto::StopAnim()), true);
    }

    bool ClientSession::SendRaw(uint8_t channel, const proto::Bytes& bytes, bool reliable)
    {
        return SendBytes(channel, bytes, reliable);
    }

    void ClientSession::Disconnect()
    {
        if (!host_) return;
        if (state_ == SessionState::Connecting)
        {
            Finish(DisconnectReason::Local, true);
            return;
        }
        if (state_ != SessionState::Joining && state_ != SessionState::Joined) return;
        // "Later" so a chat line typed just before quitting still goes out first.
        enet_peer_disconnect_later(peer_, proto::kDisconnectQuit);
        enet_host_flush(host_);
        state_ = SessionState::Closing;
        deadlineMs_ = NowMs() + opts_.closeTimeoutMs;
    }

    void ClientSession::Abandon()
    {
        Finish(DisconnectReason::Lost, false);
    }

    uint32_t ClientSession::rttMs() const
    {
        return peer_ ? peer_->roundTripTime : 0;
    }

    uint32_t ClientSession::lowRttMs() const
    {
        if (!peer_) return 0;
        uint32_t r = peer_->roundTripTime;
        if (peer_->lastRoundTripTime > 0 && peer_->lastRoundTripTime < r) r = peer_->lastRoundTripTime;
        return r;
    }

    ClientSession::LinkInfo ClientSession::link() const
    {
        LinkInfo l;
        if (!peer_) return l;
        l.rttMs = peer_->roundTripTime;
        l.rttVarMs = peer_->roundTripTimeVariance;
        l.throttle = peer_->packetThrottle;
        l.throttleScale = ENET_PEER_PACKET_THROTTLE_SCALE;
        l.throttleDeceleration = peer_->packetThrottleDeceleration;
        l.lossPercent = 100.0 * (double)peer_->packetLoss / (double)ENET_PEER_PACKET_LOSS_SCALE;
        return l;
    }

}
