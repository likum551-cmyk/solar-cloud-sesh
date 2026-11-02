import { describe, expect, it } from 'vitest';
import {
  QUERY_DATAGRAM_SIZE,
  QUERY_MAGIC,
  decodeQueryReply,
  decodeQueryRequest,
  encodeQueryReply,
  encodeQueryRequest,
} from '../src/query-protocol.js';
import { MAX_PLAYERS } from '../src/schema.js';

const NONCE = Buffer.from('0102030405060708', 'hex');

// The byte vectors in PROTOCOL.md; the C++ side should assert the same ones.
const REQUEST_HEX = '47414745' + '01' + '01' + '0000' + '0102030405060708' + '0000000000000000';
const REPLY_HEX = '47414745' + '81' + '01' + '0100' + '0102030405060708' + '0300' + '2000' + '00000000';

describe('GAGE UDP query wire format', () => {
  it('encodes the documented request bytes', () => {
    expect(encodeQueryRequest(NONCE).toString('hex')).toBe(REQUEST_HEX);
    expect(encodeQueryRequest(NONCE)).toHaveLength(24);
  });

  it('encodes the documented reply bytes (little-endian integers)', () => {
    const reply = encodeQueryReply(NONCE, { protocol: 1, players: 3, maxPlayers: 32 });
    expect(reply.toString('hex')).toBe(REPLY_HEX);
    expect(reply).toHaveLength(QUERY_DATAGRAM_SIZE);
  });

  it('reply and request have the same size, so a spoofed query cannot be amplified', () => {
    expect(encodeQueryReply(NONCE, { protocol: 65535, players: 1024, maxPlayers: 1024 })).toHaveLength(encodeQueryRequest(NONCE).length);
  });

  it('round-trips', () => {
    expect(decodeQueryRequest(encodeQueryRequest(NONCE))?.equals(NONCE)).toBe(true);
    expect(decodeQueryReply(encodeQueryReply(NONCE, { protocol: 7, players: 1, maxPlayers: 2 }), NONCE)).toEqual({ protocol: 7, players: 1, maxPlayers: 2 });
  });

  it('rejects a reply that is not exactly a reply to this nonce', () => {
    const good = encodeQueryReply(NONCE, { protocol: 1, players: 3, maxPlayers: 32 });
    expect(decodeQueryReply(good, NONCE)).not.toBeNull();

    const mutate = (i: number, v: number): Buffer => {
      const b = Buffer.from(good);
      b[i] = v;
      return b;
    };
    expect(decodeQueryReply(good.subarray(0, 23), NONCE), 'short').toBeNull();
    expect(decodeQueryReply(Buffer.concat([good, Buffer.from([0])]), NONCE), 'long').toBeNull();
    expect(decodeQueryReply(mutate(0, 0x00), NONCE), 'magic').toBeNull();
    expect(decodeQueryReply(mutate(4, 0x01), NONCE), 'kind: a request is not a reply').toBeNull();
    expect(decodeQueryReply(mutate(5, 0x02), NONCE), 'query version').toBeNull();
    expect(decodeQueryReply(mutate(8, 0xff), NONCE), 'nonce').toBeNull();
    expect(decodeQueryReply(good, Buffer.from('ffffffffffffffff', 'hex')), 'other nonce').toBeNull();
    expect(decodeQueryReply(encodeQueryRequest(NONCE), NONCE), 'the request echoed back').toBeNull();
  });

  it('rejects a request that is not exactly a request', () => {
    const good = encodeQueryRequest(NONCE);
    expect(decodeQueryRequest(good.subarray(0, 23))).toBeNull();
    expect(decodeQueryRequest(Buffer.alloc(24))).toBeNull();
    expect(decodeQueryRequest(encodeQueryReply(NONCE, { protocol: 1, players: 0, maxPlayers: 1 }))).toBeNull();
  });

  it('refuses a nonce of the wrong size', () => {
    expect(() => encodeQueryRequest(Buffer.alloc(7))).toThrow(RangeError);
    expect(() => encodeQueryReply(Buffer.alloc(9), { protocol: 1, players: 0, maxPlayers: 1 })).toThrow(RangeError);
  });
});

// PROTOCOL.md section "Why ENet cannot mistake this for its own traffic": the first two
// datagram bytes are ENetProtocolHeader.peerID, big-endian, with the two high bits as flags
// (bit 15 sent-time, bit 14 compressed), bits 13..12 the session id and bits 11..0 the peer id.
describe('the magic against ENet header rules (third_party/enet/protocol.c)', () => {
  const field = QUERY_MAGIC.readUInt16BE(0);

  it('reads as the compressed flag plus peer 1857', () => {
    expect(field).toBe(0x4741);
    expect(field & 0x8000, 'sent-time flag').toBe(0);
    expect(field & 0x4000, 'compressed flag').toBe(0x4000);
    expect((field >> 12) & 3, 'session').toBe(0);
    expect(field & 0x0fff, 'peer id').toBe(1857);
  });

  it('names a peer id the server can never have: protocol.h kMaxPlayers is 1024 and ENet 0xFFF is the connect slot', () => {
    const peerId = field & 0x0fff;
    expect(peerId).toBeGreaterThanOrEqual(MAX_PLAYERS); // a host built with peerCount <= 1024 drops it: peerID >= host->peerCount
    expect(peerId).not.toBe(0x0fff); // 0xFFF would be the "new connection" slot that ENet parses commands for
  });
});
