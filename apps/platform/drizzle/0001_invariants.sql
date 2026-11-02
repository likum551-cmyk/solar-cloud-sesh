-- Hand-written invariants that drizzle's schema DSL cannot express (see src/db/schema.ts header).
-- Statements are separated by the drizzle breakpoint marker so each runs on its own.

-- 1. Append-only tables: rows are never changed or removed once written.
CREATE FUNCTION gage_reject_mutation() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
  RAISE EXCEPTION '% is append-only (% rejected)', TG_TABLE_NAME, TG_OP USING ERRCODE = '55000';
END;
$$;
--> statement-breakpoint
CREATE TRIGGER wallet_ledger_append_only
  BEFORE UPDATE OR DELETE ON wallet_ledger
  FOR EACH ROW EXECUTE FUNCTION gage_reject_mutation();
--> statement-breakpoint
CREATE TRIGGER audit_log_append_only
  BEFORE UPDATE OR DELETE ON audit_log
  FOR EACH ROW EXECUTE FUNCTION gage_reject_mutation();
--> statement-breakpoint

-- 2. The wallet balance is derived: every ledger insert adjusts wallet_balances in the same
-- statement, and an insert that would take the balance below zero is rejected, except a
-- 'revoke' (a refund or chargeback may leave the player in debt; later purchases pay it off).
CREATE FUNCTION wallet_ledger_apply() RETURNS trigger
LANGUAGE plpgsql AS $$
DECLARE
  new_balance bigint;
BEGIN
  INSERT INTO wallet_balances (account_id, currency, balance, updated_at)
  VALUES (NEW.account_id, NEW.currency, NEW.delta, NEW.created_at)
  ON CONFLICT (account_id, currency) DO UPDATE
    SET balance = wallet_balances.balance + EXCLUDED.balance,
        updated_at = EXCLUDED.updated_at
  RETURNING balance INTO new_balance;

  IF new_balance < 0 AND NEW.reason <> 'revoke' THEN
    RAISE EXCEPTION 'insufficient_funds' USING ERRCODE = '23514';
  END IF;
  RETURN NEW;
END;
$$;
--> statement-breakpoint
CREATE TRIGGER wallet_ledger_apply
  AFTER INSERT ON wallet_ledger
  FOR EACH ROW EXECUTE FUNCTION wallet_ledger_apply();
--> statement-breakpoint

-- 3. wallet_balances is writable only from inside the ledger trigger (trigger depth 2): a direct
-- INSERT/UPDATE/DELETE from application code or a console runs at depth 1 and is rejected.
CREATE FUNCTION wallet_balances_guard() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
  IF pg_trigger_depth() < 2 THEN
    RAISE EXCEPTION 'wallet_balances is derived from wallet_ledger and cannot be written directly'
      USING ERRCODE = '55000';
  END IF;
  IF TG_OP = 'DELETE' THEN
    RETURN OLD;
  END IF;
  RETURN NEW;
END;
$$;
--> statement-breakpoint
CREATE TRIGGER wallet_balances_guard
  BEFORE INSERT OR UPDATE OR DELETE ON wallet_balances
  FOR EACH ROW EXECUTE FUNCTION wallet_balances_guard();
--> statement-breakpoint

-- 4. Order status transitions (the state machine). Same-status updates are allowed (other columns).
CREATE FUNCTION orders_status_transition() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
  IF OLD.status = NEW.status THEN
    RETURN NEW;
  END IF;
  IF (OLD.status, NEW.status) IN (
    ('pending', 'paid'),
    ('pending', 'failed'),
    ('paid', 'fulfilled'),
    ('paid', 'refunded'),
    ('paid', 'chargeback'),
    ('fulfilled', 'refunded'),
    ('fulfilled', 'chargeback')
  ) THEN
    RETURN NEW;
  END IF;
  -- A verified payment that arrives after our expiry timer or an earlier failed attempt revives the
  -- order (the customer paid). Other failures (amount_mismatch, checkout_error) stay terminal.
  IF OLD.status = 'failed' AND NEW.status = 'paid' AND OLD.failure_reason IN ('expired', 'payment_failed') THEN
    RETURN NEW;
  END IF;
  RAISE EXCEPTION 'illegal order status transition % -> %', OLD.status, NEW.status USING ERRCODE = '23514';
END;
$$;
--> statement-breakpoint
CREATE TRIGGER orders_status_transition
  BEFORE UPDATE OF status ON orders
  FOR EACH ROW EXECUTE FUNCTION orders_status_transition();
--> statement-breakpoint

-- 5. Order lines are immutable snapshots once the order exists.
CREATE TRIGGER order_items_immutable
  BEFORE UPDATE ON order_items
  FOR EACH ROW EXECUTE FUNCTION gage_reject_mutation();
