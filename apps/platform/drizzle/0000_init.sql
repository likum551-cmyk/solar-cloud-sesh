CREATE TABLE "account_roles" (
	"account_id" uuid NOT NULL,
	"role" text NOT NULL,
	"granted_at" timestamp with time zone DEFAULT now() NOT NULL,
	CONSTRAINT "account_roles_account_id_role_pk" PRIMARY KEY("account_id","role"),
	CONSTRAINT "account_roles_role" CHECK ("account_roles"."role" in ('player', 'mod', 'admin'))
);
--> statement-breakpoint
CREATE TABLE "account_settings" (
	"account_id" uuid PRIMARY KEY NOT NULL,
	"data" jsonb NOT NULL,
	"version" integer DEFAULT 1 NOT NULL,
	"updated_at" timestamp with time zone DEFAULT now() NOT NULL,
	CONSTRAINT "account_settings_object" CHECK (jsonb_typeof("account_settings"."data") = 'object')
);
--> statement-breakpoint
CREATE TABLE "accounts" (
	"id" uuid PRIMARY KEY DEFAULT gen_random_uuid() NOT NULL,
	"email" text,
	"email_verified_at" timestamp with time zone,
	"username" text NOT NULL,
	"password_hash" text,
	"password_changed_at" timestamp with time zone,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL,
	"updated_at" timestamp with time zone DEFAULT now() NOT NULL,
	"last_login_at" timestamp with time zone,
	CONSTRAINT "accounts_username_format" CHECK ("accounts"."username" ~ '^[A-Za-z0-9_]{3,20}$'),
	CONSTRAINT "accounts_email_lowercase" CHECK ("accounts"."email" is null or ("accounts"."email" = lower("accounts"."email") and length("accounts"."email") <= 254)),
	CONSTRAINT "accounts_password_needs_email" CHECK ("accounts"."password_hash" is null or "accounts"."email" is not null),
	CONSTRAINT "accounts_verified_needs_email" CHECK ("accounts"."email_verified_at" is null or "accounts"."email" is not null)
);
--> statement-breakpoint
CREATE TABLE "audit_log" (
	"id" bigint PRIMARY KEY GENERATED ALWAYS AS IDENTITY (sequence name "audit_log_id_seq" INCREMENT BY 1 MINVALUE 1 MAXVALUE 9223372036854775807 START WITH 1 CACHE 1),
	"at" timestamp with time zone DEFAULT now() NOT NULL,
	"actor_account_id" uuid,
	"actor_label" text NOT NULL,
	"action" text NOT NULL,
	"target_type" text NOT NULL,
	"target_id" text NOT NULL,
	"details" jsonb DEFAULT '{}'::jsonb NOT NULL,
	"ip" text,
	"request_id" text
);
--> statement-breakpoint
CREATE TABLE "auth_throttle" (
	"key" text PRIMARY KEY NOT NULL,
	"failures" integer DEFAULT 0 NOT NULL,
	"window_start" timestamp with time zone NOT NULL,
	"locked_until" timestamp with time zone,
	"updated_at" timestamp with time zone DEFAULT now() NOT NULL
);
--> statement-breakpoint
CREATE TABLE "auth_tokens" (
	"id" uuid PRIMARY KEY DEFAULT gen_random_uuid() NOT NULL,
	"account_id" uuid NOT NULL,
	"kind" text NOT NULL,
	"token_hash" text NOT NULL,
	"email" text,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL,
	"expires_at" timestamp with time zone NOT NULL,
	"used_at" timestamp with time zone,
	CONSTRAINT "auth_tokens_kind" CHECK ("auth_tokens"."kind" in ('email_verify', 'password_reset'))
);
--> statement-breakpoint
CREATE TABLE "bans" (
	"id" uuid PRIMARY KEY DEFAULT gen_random_uuid() NOT NULL,
	"account_id" uuid NOT NULL,
	"reason" text NOT NULL,
	"created_by" uuid,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL,
	"expires_at" timestamp with time zone,
	"revoked_at" timestamp with time zone,
	"revoked_by" uuid,
	"revoked_reason" text,
	CONSTRAINT "bans_reason_len" CHECK (length("bans"."reason") between 1 and 500)
);
--> statement-breakpoint
CREATE TABLE "email_outbox" (
	"id" uuid PRIMARY KEY DEFAULT gen_random_uuid() NOT NULL,
	"seq" bigint GENERATED ALWAYS AS IDENTITY (sequence name "email_outbox_seq_seq" INCREMENT BY 1 MINVALUE 1 MAXVALUE 9223372036854775807 START WITH 1 CACHE 1),
	"to_address" text NOT NULL,
	"subject" text NOT NULL,
	"body" text NOT NULL,
	"template" text NOT NULL,
	"meta" jsonb DEFAULT '{}'::jsonb NOT NULL,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL
);
--> statement-breakpoint
CREATE TABLE "game_servers" (
	"id" text PRIMARY KEY NOT NULL,
	"name" text NOT NULL,
	"region" text NOT NULL,
	"host" text NOT NULL,
	"port" integer NOT NULL,
	"tags" text[] DEFAULT '{}'::text[] NOT NULL,
	"sort" integer DEFAULT 0 NOT NULL,
	"visible" boolean DEFAULT true NOT NULL,
	"maintenance" boolean DEFAULT false NOT NULL,
	"maintenance_message" text,
	"secret_hash" text NOT NULL,
	"allowed_ips" text[] DEFAULT '{}'::text[] NOT NULL,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL,
	"updated_at" timestamp with time zone DEFAULT now() NOT NULL,
	CONSTRAINT "game_servers_id_slug" CHECK ("game_servers"."id" ~ '^[a-z0-9][a-z0-9-]{1,31}$'),
	CONSTRAINT "game_servers_port" CHECK ("game_servers"."port" between 1 and 65535)
);
--> statement-breakpoint
CREATE TABLE "game_sessions" (
	"id" uuid PRIMARY KEY DEFAULT gen_random_uuid() NOT NULL,
	"account_id" uuid NOT NULL,
	"server_id" text NOT NULL,
	"join_token_id" uuid,
	"started_at" timestamp with time zone DEFAULT now() NOT NULL,
	"ended_at" timestamp with time zone,
	"end_reason" text
);
--> statement-breakpoint
CREATE TABLE "grants" (
	"id" uuid PRIMARY KEY DEFAULT gen_random_uuid() NOT NULL,
	"seq" bigint GENERATED ALWAYS AS IDENTITY (sequence name "grants_seq_seq" INCREMENT BY 1 MINVALUE 1 MAXVALUE 9223372036854775807 START WITH 1 CACHE 1),
	"account_id" uuid NOT NULL,
	"order_id" uuid NOT NULL,
	"line_no" integer NOT NULL,
	"entry_no" integer NOT NULL,
	"kind" text NOT NULL,
	"revokes_grant_id" uuid,
	"type" text NOT NULL,
	"sku" text NOT NULL,
	"payload" jsonb NOT NULL,
	"quantity" integer DEFAULT 1 NOT NULL,
	"status" text DEFAULT 'pending' NOT NULL,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL,
	"acked_at" timestamp with time zone,
	"acked_by" text,
	CONSTRAINT "grants_entry_uq" UNIQUE("order_id","line_no","entry_no","kind"),
	CONSTRAINT "grants_kind" CHECK ("grants"."kind" in ('grant', 'revoke')),
	CONSTRAINT "grants_revoke_link" CHECK (("grants"."kind" = 'revoke') = ("grants"."revokes_grant_id" is not null)),
	CONSTRAINT "grants_type" CHECK ("grants"."type" in ('wallet_credit', 'entitlement', 'item')),
	CONSTRAINT "grants_status" CHECK ("grants"."status" in ('pending', 'acked')),
	CONSTRAINT "grants_acked_has_time" CHECK (("grants"."status" = 'acked') = ("grants"."acked_at" is not null)),
	CONSTRAINT "grants_quantity" CHECK ("grants"."quantity" between 1 and 100)
);
--> statement-breakpoint
CREATE TABLE "join_tokens" (
	"id" uuid PRIMARY KEY DEFAULT gen_random_uuid() NOT NULL,
	"token_hash" text NOT NULL,
	"account_id" uuid NOT NULL,
	"server_id" text NOT NULL,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL,
	"expires_at" timestamp with time zone NOT NULL,
	"used_at" timestamp with time zone,
	"ip" text,
	CONSTRAINT "join_tokens_expiry_after_create" CHECK ("join_tokens"."expires_at" > "join_tokens"."created_at")
);
--> statement-breakpoint
CREATE TABLE "oauth_flows" (
	"id" uuid PRIMARY KEY DEFAULT gen_random_uuid() NOT NULL,
	"provider" text NOT NULL,
	"intent" text NOT NULL,
	"state_hash" text NOT NULL,
	"code_verifier" text,
	"poll_secret_hash" text NOT NULL,
	"account_id" uuid,
	"status" text DEFAULT 'pending' NOT NULL,
	"result_account_id" uuid,
	"profile" jsonb,
	"error_code" text,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL,
	"expires_at" timestamp with time zone NOT NULL,
	"callback_at" timestamp with time zone,
	"consumed_at" timestamp with time zone,
	CONSTRAINT "oauth_flows_intent" CHECK ("oauth_flows"."intent" in ('login', 'link')),
	CONSTRAINT "oauth_flows_status" CHECK ("oauth_flows"."status" in ('pending', 'login_ready', 'needs_username', 'linked', 'failed', 'consumed')),
	CONSTRAINT "oauth_flows_link_needs_account" CHECK ("oauth_flows"."intent" <> 'link' or "oauth_flows"."account_id" is not null)
);
--> statement-breakpoint
CREATE TABLE "oauth_identities" (
	"id" uuid PRIMARY KEY DEFAULT gen_random_uuid() NOT NULL,
	"account_id" uuid NOT NULL,
	"provider" text NOT NULL,
	"provider_user_id" text NOT NULL,
	"provider_username" text,
	"provider_email" text,
	"email_verified" boolean DEFAULT false NOT NULL,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL,
	CONSTRAINT "oauth_identities_provider_user_uq" UNIQUE("provider","provider_user_id"),
	CONSTRAINT "oauth_identities_account_provider_uq" UNIQUE("account_id","provider"),
	CONSTRAINT "oauth_identities_provider" CHECK ("oauth_identities"."provider" in ('discord'))
);
--> statement-breakpoint
CREATE TABLE "order_items" (
	"order_id" uuid NOT NULL,
	"line_no" integer NOT NULL,
	"product_id" uuid NOT NULL,
	"sku" text NOT NULL,
	"name" text NOT NULL,
	"kind" text NOT NULL,
	"unit_price_minor" bigint NOT NULL,
	"currency" text NOT NULL,
	"quantity" integer NOT NULL,
	"grant_payload" jsonb NOT NULL,
	CONSTRAINT "order_items_order_id_line_no_pk" PRIMARY KEY("order_id","line_no"),
	CONSTRAINT "order_items_quantity" CHECK ("order_items"."quantity" between 1 and 100),
	CONSTRAINT "order_items_price" CHECK ("order_items"."unit_price_minor" > 0)
);
--> statement-breakpoint
CREATE TABLE "orders" (
	"id" uuid PRIMARY KEY DEFAULT gen_random_uuid() NOT NULL,
	"account_id" uuid NOT NULL,
	"status" text DEFAULT 'pending' NOT NULL,
	"currency" text NOT NULL,
	"total_minor" bigint NOT NULL,
	"provider" text NOT NULL,
	"provider_ref" text,
	"checkout_url" text,
	"idempotency_key" text,
	"failure_reason" text,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL,
	"updated_at" timestamp with time zone DEFAULT now() NOT NULL,
	"expires_at" timestamp with time zone NOT NULL,
	"paid_at" timestamp with time zone,
	"fulfilled_at" timestamp with time zone,
	"refunded_at" timestamp with time zone,
	CONSTRAINT "orders_id_currency_uq" UNIQUE("id","currency"),
	CONSTRAINT "orders_status" CHECK ("orders"."status" in ('pending', 'paid', 'fulfilled', 'refunded', 'failed', 'chargeback')),
	CONSTRAINT "orders_total_positive" CHECK ("orders"."total_minor" > 0),
	CONSTRAINT "orders_currency_iso" CHECK ("orders"."currency" ~ '^[A-Z]{3}$'),
	CONSTRAINT "orders_paid_has_paid_at" CHECK ("orders"."status" in ('pending', 'failed') or "orders"."paid_at" is not null),
	CONSTRAINT "orders_fulfilled_has_time" CHECK ("orders"."status" <> 'fulfilled' or "orders"."fulfilled_at" is not null)
);
--> statement-breakpoint
CREATE TABLE "payment_events" (
	"id" uuid PRIMARY KEY DEFAULT gen_random_uuid() NOT NULL,
	"provider" text NOT NULL,
	"event_id" text NOT NULL,
	"type" text NOT NULL,
	"order_id" uuid,
	"result" text NOT NULL,
	"detail" text,
	"payload" jsonb NOT NULL,
	"received_at" timestamp with time zone DEFAULT now() NOT NULL,
	CONSTRAINT "payment_events_provider_event_uq" UNIQUE("provider","event_id"),
	CONSTRAINT "payment_events_result" CHECK ("payment_events"."result" in ('applied', 'ignored', 'unmatched', 'rejected'))
);
--> statement-breakpoint
CREATE TABLE "products" (
	"id" uuid PRIMARY KEY DEFAULT gen_random_uuid() NOT NULL,
	"sku" text NOT NULL,
	"name" text NOT NULL,
	"description" text DEFAULT '' NOT NULL,
	"price_minor" bigint NOT NULL,
	"currency" text NOT NULL,
	"kind" text NOT NULL,
	"grant_payload" jsonb NOT NULL,
	"active" boolean DEFAULT true NOT NULL,
	"sort" integer DEFAULT 0 NOT NULL,
	"image_url" text,
	"max_quantity" integer DEFAULT 1 NOT NULL,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL,
	"updated_at" timestamp with time zone DEFAULT now() NOT NULL,
	CONSTRAINT "products_sku_format" CHECK ("products"."sku" ~ '^[a-z0-9][a-z0-9_.-]{1,62}$'),
	CONSTRAINT "products_price_positive" CHECK ("products"."price_minor" > 0 and "products"."price_minor" <= 100000000),
	CONSTRAINT "products_currency_iso" CHECK ("products"."currency" ~ '^[A-Z]{3}$'),
	CONSTRAINT "products_kind" CHECK ("products"."kind" in ('currency_pack', 'vip', 'cosmetic', 'bundle')),
	CONSTRAINT "products_max_quantity" CHECK ("products"."max_quantity" between 1 and 100)
);
--> statement-breakpoint
CREATE TABLE "rate_limits" (
	"key" text PRIMARY KEY NOT NULL,
	"window_start" timestamp with time zone NOT NULL,
	"count" integer NOT NULL
);
--> statement-breakpoint
CREATE TABLE "sessions" (
	"id" uuid PRIMARY KEY DEFAULT gen_random_uuid() NOT NULL,
	"account_id" uuid NOT NULL,
	"token_hash" text NOT NULL,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL,
	"last_seen_at" timestamp with time zone DEFAULT now() NOT NULL,
	"expires_at" timestamp with time zone NOT NULL,
	"absolute_expires_at" timestamp with time zone NOT NULL,
	"user_agent" text,
	"device_name" text,
	"ip" text,
	"revoked_at" timestamp with time zone,
	"revoked_reason" text,
	CONSTRAINT "sessions_expiry_order" CHECK ("sessions"."expires_at" <= "sessions"."absolute_expires_at")
);
--> statement-breakpoint
CREATE TABLE "wallet_balances" (
	"account_id" uuid NOT NULL,
	"currency" text NOT NULL,
	"balance" bigint NOT NULL,
	"updated_at" timestamp with time zone DEFAULT now() NOT NULL,
	CONSTRAINT "wallet_balances_account_id_currency_pk" PRIMARY KEY("account_id","currency")
);
--> statement-breakpoint
CREATE TABLE "wallet_ledger" (
	"id" bigint PRIMARY KEY GENERATED ALWAYS AS IDENTITY (sequence name "wallet_ledger_id_seq" INCREMENT BY 1 MINVALUE 1 MAXVALUE 9223372036854775807 START WITH 1 CACHE 1),
	"account_id" uuid NOT NULL,
	"currency" text NOT NULL,
	"delta" bigint NOT NULL,
	"reason" text NOT NULL,
	"idempotency_key" text NOT NULL,
	"grant_id" uuid,
	"server_id" text,
	"memo" text,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL,
	CONSTRAINT "wallet_ledger_delta_nonzero" CHECK ("wallet_ledger"."delta" <> 0),
	CONSTRAINT "wallet_ledger_currency" CHECK ("wallet_ledger"."currency" ~ '^[a-z][a-z0-9_]{1,15}$'),
	CONSTRAINT "wallet_ledger_reason" CHECK ("wallet_ledger"."reason" in ('purchase', 'revoke', 'spend')),
	CONSTRAINT "wallet_ledger_sign" CHECK (("wallet_ledger"."reason" = 'purchase' and "wallet_ledger"."delta" > 0) or ("wallet_ledger"."reason" in ('revoke', 'spend') and "wallet_ledger"."delta" < 0))
);
--> statement-breakpoint
ALTER TABLE "account_roles" ADD CONSTRAINT "account_roles_account_id_accounts_id_fk" FOREIGN KEY ("account_id") REFERENCES "public"."accounts"("id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "account_settings" ADD CONSTRAINT "account_settings_account_id_accounts_id_fk" FOREIGN KEY ("account_id") REFERENCES "public"."accounts"("id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "auth_tokens" ADD CONSTRAINT "auth_tokens_account_id_accounts_id_fk" FOREIGN KEY ("account_id") REFERENCES "public"."accounts"("id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "bans" ADD CONSTRAINT "bans_account_id_accounts_id_fk" FOREIGN KEY ("account_id") REFERENCES "public"."accounts"("id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "bans" ADD CONSTRAINT "bans_created_by_accounts_id_fk" FOREIGN KEY ("created_by") REFERENCES "public"."accounts"("id") ON DELETE set null ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "bans" ADD CONSTRAINT "bans_revoked_by_accounts_id_fk" FOREIGN KEY ("revoked_by") REFERENCES "public"."accounts"("id") ON DELETE set null ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "game_sessions" ADD CONSTRAINT "game_sessions_account_id_accounts_id_fk" FOREIGN KEY ("account_id") REFERENCES "public"."accounts"("id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "game_sessions" ADD CONSTRAINT "game_sessions_server_id_game_servers_id_fk" FOREIGN KEY ("server_id") REFERENCES "public"."game_servers"("id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "game_sessions" ADD CONSTRAINT "game_sessions_join_token_id_join_tokens_id_fk" FOREIGN KEY ("join_token_id") REFERENCES "public"."join_tokens"("id") ON DELETE set null ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "grants" ADD CONSTRAINT "grants_account_id_accounts_id_fk" FOREIGN KEY ("account_id") REFERENCES "public"."accounts"("id") ON DELETE restrict ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "grants" ADD CONSTRAINT "grants_order_id_orders_id_fk" FOREIGN KEY ("order_id") REFERENCES "public"."orders"("id") ON DELETE restrict ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "grants" ADD CONSTRAINT "grants_revokes_fk" FOREIGN KEY ("revokes_grant_id") REFERENCES "public"."grants"("id") ON DELETE no action ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "join_tokens" ADD CONSTRAINT "join_tokens_account_id_accounts_id_fk" FOREIGN KEY ("account_id") REFERENCES "public"."accounts"("id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "join_tokens" ADD CONSTRAINT "join_tokens_server_id_game_servers_id_fk" FOREIGN KEY ("server_id") REFERENCES "public"."game_servers"("id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "oauth_flows" ADD CONSTRAINT "oauth_flows_account_id_accounts_id_fk" FOREIGN KEY ("account_id") REFERENCES "public"."accounts"("id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "oauth_flows" ADD CONSTRAINT "oauth_flows_result_account_id_accounts_id_fk" FOREIGN KEY ("result_account_id") REFERENCES "public"."accounts"("id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "oauth_identities" ADD CONSTRAINT "oauth_identities_account_id_accounts_id_fk" FOREIGN KEY ("account_id") REFERENCES "public"."accounts"("id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "order_items" ADD CONSTRAINT "order_items_product_id_products_id_fk" FOREIGN KEY ("product_id") REFERENCES "public"."products"("id") ON DELETE restrict ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "order_items" ADD CONSTRAINT "order_items_order_currency_fk" FOREIGN KEY ("order_id","currency") REFERENCES "public"."orders"("id","currency") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "orders" ADD CONSTRAINT "orders_account_id_accounts_id_fk" FOREIGN KEY ("account_id") REFERENCES "public"."accounts"("id") ON DELETE restrict ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "sessions" ADD CONSTRAINT "sessions_account_id_accounts_id_fk" FOREIGN KEY ("account_id") REFERENCES "public"."accounts"("id") ON DELETE cascade ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "wallet_balances" ADD CONSTRAINT "wallet_balances_account_id_accounts_id_fk" FOREIGN KEY ("account_id") REFERENCES "public"."accounts"("id") ON DELETE restrict ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "wallet_ledger" ADD CONSTRAINT "wallet_ledger_account_id_accounts_id_fk" FOREIGN KEY ("account_id") REFERENCES "public"."accounts"("id") ON DELETE restrict ON UPDATE no action;--> statement-breakpoint
ALTER TABLE "wallet_ledger" ADD CONSTRAINT "wallet_ledger_grant_id_grants_id_fk" FOREIGN KEY ("grant_id") REFERENCES "public"."grants"("id") ON DELETE restrict ON UPDATE no action;--> statement-breakpoint
CREATE UNIQUE INDEX "accounts_username_lower_uq" ON "accounts" USING btree (lower("username"));--> statement-breakpoint
CREATE UNIQUE INDEX "accounts_email_lower_uq" ON "accounts" USING btree (lower("email")) WHERE "accounts"."email" is not null;--> statement-breakpoint
CREATE INDEX "audit_log_at_idx" ON "audit_log" USING btree ("at");--> statement-breakpoint
CREATE INDEX "audit_log_target_idx" ON "audit_log" USING btree ("target_type","target_id");--> statement-breakpoint
CREATE UNIQUE INDEX "auth_tokens_hash_uq" ON "auth_tokens" USING btree ("token_hash");--> statement-breakpoint
CREATE INDEX "auth_tokens_account_kind_idx" ON "auth_tokens" USING btree ("account_id","kind");--> statement-breakpoint
CREATE UNIQUE INDEX "bans_one_open_per_account" ON "bans" USING btree ("account_id") WHERE "bans"."revoked_at" is null;--> statement-breakpoint
CREATE INDEX "email_outbox_to_idx" ON "email_outbox" USING btree ("to_address","created_at");--> statement-breakpoint
CREATE UNIQUE INDEX "game_sessions_one_open_per_account" ON "game_sessions" USING btree ("account_id") WHERE "game_sessions"."ended_at" is null;--> statement-breakpoint
CREATE INDEX "game_sessions_server_open_idx" ON "game_sessions" USING btree ("server_id") WHERE "game_sessions"."ended_at" is null;--> statement-breakpoint
CREATE UNIQUE INDEX "grants_seq_uq" ON "grants" USING btree ("seq");--> statement-breakpoint
CREATE UNIQUE INDEX "grants_one_revoke_per_grant" ON "grants" USING btree ("revokes_grant_id") WHERE "grants"."revokes_grant_id" is not null;--> statement-breakpoint
CREATE INDEX "grants_pending_idx" ON "grants" USING btree ("account_id","seq") WHERE "grants"."status" = 'pending';--> statement-breakpoint
CREATE INDEX "grants_order_idx" ON "grants" USING btree ("order_id");--> statement-breakpoint
CREATE UNIQUE INDEX "join_tokens_hash_uq" ON "join_tokens" USING btree ("token_hash");--> statement-breakpoint
CREATE INDEX "join_tokens_account_idx" ON "join_tokens" USING btree ("account_id");--> statement-breakpoint
CREATE UNIQUE INDEX "oauth_flows_state_uq" ON "oauth_flows" USING btree ("state_hash");--> statement-breakpoint
CREATE UNIQUE INDEX "orders_idempotency_uq" ON "orders" USING btree ("account_id","idempotency_key") WHERE "orders"."idempotency_key" is not null;--> statement-breakpoint
CREATE UNIQUE INDEX "orders_provider_ref_uq" ON "orders" USING btree ("provider","provider_ref") WHERE "orders"."provider_ref" is not null;--> statement-breakpoint
CREATE INDEX "orders_account_idx" ON "orders" USING btree ("account_id","created_at");--> statement-breakpoint
CREATE INDEX "payment_events_order_idx" ON "payment_events" USING btree ("order_id");--> statement-breakpoint
CREATE UNIQUE INDEX "products_sku_uq" ON "products" USING btree ("sku");--> statement-breakpoint
CREATE UNIQUE INDEX "sessions_token_hash_uq" ON "sessions" USING btree ("token_hash");--> statement-breakpoint
CREATE INDEX "sessions_account_idx" ON "sessions" USING btree ("account_id");--> statement-breakpoint
CREATE UNIQUE INDEX "wallet_ledger_idem_uq" ON "wallet_ledger" USING btree ("idempotency_key");--> statement-breakpoint
CREATE INDEX "wallet_ledger_account_idx" ON "wallet_ledger" USING btree ("account_id","currency","id");