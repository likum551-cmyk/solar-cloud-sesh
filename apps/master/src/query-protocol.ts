// GAGE UDP query: wire format of the probe the master (and later the launcher) sends to a
// game server's UDP port, and of the server's reply. Specified in PROTOCOL.md; the constants
// and layouts here are the executable copy the tests pin with fixed byte vectors.
//
// Both datagrams are exactly 24 bytes (a reply can never be larger than the request that
// triggered it, so the server is useless as a spoofed-source amplifier). Multi-byte integers
// are little-endian, as in the game protocol (docs/ENGINE-PLAN.md).
//
//   request : "GAGE" | u8 kind=0x01 | u8 queryVersion=1 | u16 0      | u8[8] nonce | u8[8] zero
//   reply   : "GAGE" | u8 kind=0x81 | u8 queryVersion=1 | u16 protocol | u8[8] nonce | u16 players
//             | u16 maxPlayers | u32 0

/** ASCII "GAGE". As the first two bytes of an ENet datagram this reads as peerID field 0x4741: COMPRESSED flag set, peer 1857 (PROTOCOL.md explains why ENet can never produce or accept it). */
export const QUERY_MAGIC = Buffer.from([0x47, 0x41, 0x47, 0x45]);
export const QUERY_KIND_REQUEST = 0x01;
export const QUERY_KIND_REPLY = 0x81;
export const QUERY_VERSION = 1;
export const QUERY_DATAGRAM_SIZE = 24;
export const QUERY_NONCE_SIZE = 8;

export interface QueryReply {
  /** The game protocol version the server speaks (proto::kVersion). */
  protocol: number;
  players: number;
  maxPlayers: number;
}

function header(kind: number): Buffer {
  const buf = Buffer.alloc(QUERY_DATAGRAM_SIZE);
  QUERY_MAGIC.copy(buf, 0);
  buf.writeUInt8(kind, 4);
  buf.writeUInt8(QUERY_VERSION, 5);
  return buf;
}

export function encodeQueryRequest(nonce: Buffer): Buffer {
  if (nonce.length !== QUERY_NONCE_SIZE) throw new RangeError(`nonce must be ${QUERY_NONCE_SIZE} bytes`);
  const buf = header(QUERY_KIND_REQUEST);
  nonce.copy(buf, 8);
  return buf;
}

/** The nonce of a well-formed request, or null. (What a server does; the tests' fake responder uses it.) */
export function decodeQueryRequest(buf: Buffer): Buffer | null {
  if (buf.length !== QUERY_DATAGRAM_SIZE) return null;
  if (!buf.subarray(0, 4).equals(QUERY_MAGIC)) return null;
  if (buf.readUInt8(4) !== QUERY_KIND_REQUEST || buf.readUInt8(5) !== QUERY_VERSION) return null;
  return Buffer.from(buf.subarray(8, 16));
}

export function encodeQueryReply(nonce: Buffer, reply: QueryReply): Buffer {
  if (nonce.length !== QUERY_NONCE_SIZE) throw new RangeError(`nonce must be ${QUERY_NONCE_SIZE} bytes`);
  const buf = header(QUERY_KIND_REPLY);
  buf.writeUInt16LE(reply.protocol, 6);
  nonce.copy(buf, 8);
  buf.writeUInt16LE(reply.players, 16);
  buf.writeUInt16LE(reply.maxPlayers, 18);
  return buf;
}

/** The reply, or null for anything that is not a well-formed reply to a request carrying `nonce`. */
export function decodeQueryReply(buf: Buffer, nonce: Buffer): QueryReply | null {
  if (buf.length !== QUERY_DATAGRAM_SIZE) return null;
  if (!buf.subarray(0, 4).equals(QUERY_MAGIC)) return null;
  if (buf.readUInt8(4) !== QUERY_KIND_REPLY || buf.readUInt8(5) !== QUERY_VERSION) return null;
  if (!buf.subarray(8, 16).equals(nonce)) return null;
  return {
    protocol: buf.readUInt16LE(6),
    players: buf.readUInt16LE(16),
    maxPlayers: buf.readUInt16LE(18),
  };
}
