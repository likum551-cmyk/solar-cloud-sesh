import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { sql } from 'drizzle-orm';
import { openDatabase, schema, type DbHandle } from '../src/db/index.js';
import { isCheckViolation, pgErrorInfo } from '../src/util/errors.js';

// Database-level invariants, proven against real Postgres semantics (PGlite): each rule is attacked
// with a statement the application would never send.

let handle: DbHandle;
beforeAll(async () => {
  handle = await openDatabase('pglite:memory');
  await handle.migrate();
});
afterAll(async () => {
  await handle.close();
});

const db = () => handle.db;

async function newAccount(username: string): Promise<string> {
  const [row] = await db()
    .insert(schema.accounts)
    .values({ username, email: `${username.toLowerCase()}@example.test` })
    .returning({ id: schema.accounts.id });
  return (row as { id: string }).id;
}

describe('account constraints', () => {
  it('usernames are unique case-insensitively', async () => {
    await newAccount('Alice_1');
    await expect(newAccount('alice_1')).rejects.toSatisfy((e) => pgErrorInfo(e)?.code === '23505');
  });

  it('rejects usernames outside [A-Za-z0-9_]{3,20}', async () => {
    for (const bad of ['ab', 'a'.repeat(21), 'has space', 'dash-name', 'ünïcode']) {
      await expect(newAccount(bad)).rejects.toSatisfy((e) => isCheckViolation(e));
    }
  });

  it('emails are stored lower-case and unique', async () => {
    await expect(
      db().insert(schema.accounts).values({ username: 'MixedMail', email: 'Mixed@Example.test' }),
    ).rejects.toSatisfy((e) => isCheckViolation(e));
    await db().insert(schema.accounts).values({ username: 'MailOne', email: 'one@example.test' });
    await expect(
      db().insert(schema.accounts).values({ username: 'MailTwo', email: 'one@example.test' }),
    ).rejects.toSatisfy((e) => pgErrorInfo(e)?.code === '23505');
  });

  it('allows several accounts without an email (NULL is not a duplicate)', async () => {
    await db().insert(schema.accounts).values({ username: 'NoMailA' });
    await db().insert(schema.accounts).values({ username: 'NoMailB' });
  });
});

describe('wallet ledger', () => {
  it('derives the balance by trigger, rejects overdraw, allows revoke debt, and is append-only', async () => {
    const id = await newAccount('WalletUser');
    const credit = (key: string, delta: number, reason: 'purchase' | 'spend' | 'revoke') =>
      db().insert(schema.walletLedger).values({ accountId: id, currency: 'coins', delta, reason, idempotencyKey: key });
    const balance = async () => {
      const rows = await db().select().from(schema.walletBalances).where(sql`${schema.walletBalances.accountId} = ${id}`);
      return rows[0]?.balance;
    };

    await credit('k1', 500, 'purchase');
    expect(await balance()).toBe(500);
    await credit('k2', -200, 'spend');
    expect(await balance()).toBe(300);

    await expect(credit('k3', -301, 'spend')).rejects.toSatisfy((e) => isCheckViolation(e, 'insufficient_funds'));
    expect(await balance()).toBe(300); // the failed statement rolled back, ledger and balance together

    await credit('k4', -1000, 'revoke'); // refund after the coins were spent: debt, not an error
    expect(await balance()).toBe(-700);
    await expect(credit('k5', -1, 'spend')).rejects.toSatisfy((e) => isCheckViolation(e, 'insufficient_funds'));

    await expect(credit('k1', 5, 'purchase')).rejects.toSatisfy((e) => pgErrorInfo(e)?.code === '23505');

    await expect(db().execute(sql`update wallet_ledger set delta = 999999 where idempotency_key = 'k1'`)).rejects.toSatisfy(
      (e) => pgErrorInfo(e)?.code === '55000',
    );
    await expect(db().execute(sql`delete from wallet_ledger where idempotency_key = 'k1'`)).rejects.toSatisfy(
      (e) => pgErrorInfo(e)?.code === '55000',
    );
    await expect(
      db().execute(sql`update wallet_balances set balance = 1000000 where account_id = ${id}`),
    ).rejects.toSatisfy((e) => pgErrorInfo(e)?.code === '55000');

    const sum = await db().execute(
      sql`select coalesce(sum(delta), 0)::int as total from wallet_ledger where account_id = ${id} and currency = 'coins'`,
    );
    expect((sum as unknown as { rows: { total: number }[] }).rows[0]?.total).toBe(await balance());
  });
});
