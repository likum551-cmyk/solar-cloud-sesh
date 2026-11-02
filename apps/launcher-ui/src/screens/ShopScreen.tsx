import { useCallback, useEffect, useRef, useState } from 'react';
import { newIdempotencyKey } from '../lib/api';
import { describeFailure, isFailure } from '../lib/errors';
import type { Balance, Order, OwnedItem, Product } from '../lib/schemas';
import { productStill, thumbUrl, type StillName } from '../media/catalog';
import { useApp } from '../state/AppState';
import { formatMoney, kindLabel, spentByCurrency, stampFor, vipWindow, type StampTone } from '../state/shop';
import { VipBar } from './ProfileScreen';
import { useLenis } from '../ui/useLenis';
import { useNow } from '../ui/hooks';
import { Chip, Code, Icon, Plate, type ChipTone } from '../ui/primitives';

// The shop: the chosen item's GTA V still fills the window (App/Backdrop), its name plate sits
// bottom left (name, one honest line, the price from the API, BUY), and the catalogue is a
// column of screenshot cards on the right. Checkout runs in the system browser; Orders and
// Owned are opaque panels over the dimmed still.

type View = 'showroom' | 'orders' | 'owned';

interface ShopData {
  products: Product[] | null;
  orders: Order[] | null;
  items: OwnedItem[] | null;
  balances: Balance[] | null;
  error: string | null;
}

const chipTone = (t: StampTone): ChipTone => (t === 'stamp' ? 'done' : t);
const fmtBalance = (n: number) => n.toLocaleString('en').replace(/,/g, ' ');

export function ShopScreen({ onStill }: { onStill: (s: StillName) => void }) {
  const { api, demo, openExternal, auth } = useApp();
  const [view, setView] = useState<View>('showroom');
  const [data, setData] = useState<ShopData>({ products: null, orders: null, items: null, balances: null, error: null });
  const [selected, setSelected] = useState<string | null>(null);

  const load = useCallback(async () => {
    const results = await Promise.allSettled([api.products(), api.orders(), api.items(), api.wallet()]);
    const [p, o, i, w] = results;
    const firstErr = results.find((r) => r.status === 'rejected') as PromiseRejectedResult | undefined;
    setData({
      products: p.status === 'fulfilled' ? [...p.value.products].sort((a, b) => a.sort - b.sort) : null,
      // Newest first, whatever order the API lists them in.
      orders: o.status === 'fulfilled' ? [...o.value.orders].sort((a, b) => Date.parse(b.createdAt) - Date.parse(a.createdAt)) : null,
      items: i.status === 'fulfilled' ? i.value.items : null,
      balances: w.status === 'fulfilled' ? w.value.balances : null,
      error: firstErr ? describeFailure(firstErr.reason) : null,
    });
  }, [api]);
  useEffect(() => { void load(); }, [load]);

  const products = data.products ?? [];
  const product = products.find((p) => p.id === selected) ?? products[0] ?? null;
  // The window shows the chosen product's world; an empty or unreadable catalogue shows the
  // city (never a car that is not for sale). While the first load runs, the last still stays.
  const catalogueSettled = data.products !== null || data.error !== null;
  useEffect(() => {
    if (product) onStill(productStill(product));
    else if (catalogueSettled) onStill('downtown');
  }, [product, catalogueSettled, onStill]);
  const verified = auth.status === 'signedIn' && auth.account.emailVerified;

  const onListKey = (e: React.KeyboardEvent, i: number) => {
    const fwd = e.key === 'ArrowDown' || e.key === 'ArrowRight';
    const back = e.key === 'ArrowUp' || e.key === 'ArrowLeft';
    if (!fwd && !back) return;
    e.preventDefault();
    const next = products[(i + (fwd ? 1 : products.length - 1)) % products.length];
    if (next) {
      setSelected(next.id);
      (document.querySelector(`[data-frame="${next.id}"]`) as HTMLElement | null)?.focus();
    }
  };

  return (
    <div className={`shop ${view === 'showroom' ? '' : 'shop--record'}`}>
      <div className="shop__bar">
        <nav className="subnav" aria-label="Shop">
          {(['showroom', 'orders', 'owned'] as const).map((v) => (
            <button key={v} type="button" className={`subnav__item ${view === v ? 'is-active' : ''}`} aria-current={view === v ? 'page' : undefined} onClick={() => setView(v)}>
              {v === 'showroom' ? 'Showroom' : v === 'orders' ? 'Orders' : 'Owned'}
            </button>
          ))}
        </nav>
        {/* On Orders and Owned the summary column carries the wallet; showing it here too doubled it. */}
        {view === 'showroom' && (
          <div className="wallet" aria-label="Wallet">
            <span className="wallet__label">Wallet</span>
            {data.balances === null ? <span className="wallet__n">—</span> : data.balances.length === 0 ? <span className="wallet__n">0</span> : data.balances.map((b) => (
              <span key={b.currency} className="wallet__bal"><span className="wallet__n">{fmtBalance(b.balance)}</span> <Code>{b.currency}</Code></span>
            ))}
            {demo && <Chip tone="demo">Demo</Chip>}
          </div>
        )}
      </div>

      {view === 'showroom' && (
        <div className="showroom">
          <section className="shop__plate" aria-live="polite">
            {data.products === null && !data.error && <div className="nameplate"><h1 className="nameplate__name nameplate__name--quiet">Opening the shop</h1></div>}
            {data.products === null && data.error && (
              <div className="nameplate">
                <h1 className="nameplate__name nameplate__name--quiet"><Icon name="alert" size={28} /> Shop unavailable</h1>
                <p className="nameplate__line">{data.error}</p>
                <Plate tone="ghost" onClick={() => void load()}><Icon name="refresh" /> Try again</Plate>
              </div>
            )}
            {data.products !== null && products.length === 0 && (
              <div className="nameplate">
                <h1 className="nameplate__name nameplate__name--quiet">Nothing on sale</h1>
                <p className="nameplate__line">The shop is empty right now. New items show up here as soon as they go on sale.</p>
              </div>
            )}
            {product && <ProductPlate key={product.id} product={product} demo={demo} verified={verified} onOrdered={() => void load()} openExternal={openExternal} />}
          </section>
          {products.length > 0 && (
            <section className="products" aria-label="Products">
              <header className="servers__head sr-only">
                <h2 className="servers__title">On sale</h2>
                <span className="servers__count">{products.length}</span>
              </header>
              <div className="servers__list" role="listbox" aria-label="Products" aria-orientation="vertical">
                {products.map((p, i) => {
                  const chosen = p.id === product?.id;
                  return (
                    <button
                      key={p.id}
                      data-frame={p.id}
                      type="button"
                      role="option"
                      aria-selected={chosen}
                      tabIndex={chosen ? 0 : -1}
                      className={`scard scard--product ${chosen ? 'is-chosen' : ''}`}
                      onClick={() => setSelected(p.id)}
                      onKeyDown={(e) => onListKey(e, i)}
                    >
                      <img className="scard__img" src={thumbUrl(productStill(p))} alt="" decoding="async" />
                      <span className="scard__body">
                        <span className="scard__name">{p.name}</span>
                        <span className="scard__meta">{kindLabel(p.kind, p)}</span>
                      </span>
                      <span className="scard__price">{formatMoney(p.priceMinor, p.currency)}</span>
                    </button>
                  );
                })}
              </div>
            </section>
          )}
        </div>
      )}
      {view === 'orders' && (
        <div className="ledger">
          <LedgerSummary orders={data.orders} items={data.items} balances={data.balances} demo={demo} />
          <OrdersRecord orders={data.orders} error={data.error} onReload={() => void load()} openExternal={openExternal} />
        </div>
      )}
      {view === 'owned' && (
        <div className="ledger">
          <LedgerSummary orders={data.orders} items={data.items} balances={data.balances} demo={demo} />
          <OwnedRecord items={data.items} products={products} error={data.error} onShowroom={() => setView('showroom')} />
        </div>
      )}
    </div>
  );
}

function ProductPlate({ product, demo, verified, onOrdered, openExternal }: { product: Product; demo: boolean; verified: boolean; onOrdered: () => void; openExternal: (u: string) => void }) {
  const { api } = useApp();
  const [state, setState] = useState<{ phase: 'idle' | 'creating' | 'waiting' | 'error'; order?: Order; error?: string }>({ phase: 'idle' });
  const keyRef = useRef<string | null>(null);
  const price = formatMoney(product.priceMinor, product.currency);

  // While the provider page is open, poll the order until it settles.
  useEffect(() => {
    if (state.phase !== 'waiting' || !state.order) return;
    const id = state.order.id;
    const t = setInterval(async () => {
      try {
        const { order } = await api.order(id);
        if (order.status !== 'pending') {
          setState({ phase: 'idle', order });
          keyRef.current = null;
          onOrdered();
        }
      } catch { /* keep polling; the record view shows the final state either way */ }
    }, 4000);
    return () => clearInterval(t);
  }, [state.phase, state.order, api, onOrdered]);

  const buy = async () => {
    keyRef.current ??= newIdempotencyKey();
    setState({ phase: 'creating' });
    try {
      const { order } = await api.checkout(product.sku, 1, keyRef.current);
      setState({ phase: 'waiting', order });
      if (order.checkoutUrl) openExternal(order.checkoutUrl);
    } catch (e) {
      setState({ phase: 'error', error: describeFailure(e) });
      if (!isFailure(e, 'offline', 'timeout')) keyRef.current = null;
    }
  };

  const done = state.phase === 'idle' && state.order ? stampFor(state.order, Date.now()) : null;
  return (
    <div className="nameplate">
      <h1 className="nameplate__name reveal">{product.name}</h1>
      <p className="nameplate__line">{product.description}</p>
      <div className="nameplate__price">
        <span className="price">{price}</span>
        <Chip>{kindLabel(product.kind, product)}</Chip>
        {demo && <Chip tone="demo">Demo price · not for sale</Chip>}
      </div>
      {state.phase === 'waiting' && state.order ? (
        <div className="checkout-wait" role="status">
          <p><strong>Finish paying in your browser.</strong> This order shows <Chip tone="done">Paid</Chip> here as soon as the payment provider confirms it.</p>
          <div className="checkout-wait__row">
            <Code>Order {state.order.id}</Code>
            {state.order.checkoutUrl && <button type="button" className="text-link" onClick={() => openExternal(state.order!.checkoutUrl!)}>Open checkout again <Icon name="external" /></button>}
          </div>
        </div>
      ) : (
        <>
          <Plate tone="primary" size="lg" className="buy-plate" busy={state.phase === 'creating'} disabled={!verified} onClick={() => void buy()}>
            Buy <Icon name="external" />
          </Plate>
          {!verified && <p className="nameplate__fine">Verify your email to buy. The link is in your inbox; resend it from your profile.</p>}
          {state.phase === 'error' && <p className="nameplate__error" role="alert">{state.error}</p>}
          {done && <p className="nameplate__fine">Last order: <Chip tone={chipTone(done.tone)}>{done.label}</Chip></p>}
        </>
      )}
      <p className="nameplate__fine">Payment happens on the provider’s page in your browser. Solar RP never sees your card.</p>
    </div>
  );
}

const MONTHS = ['JAN', 'FEB', 'MAR', 'APR', 'MAY', 'JUN', 'JUL', 'AUG', 'SEP', 'OCT', 'NOV', 'DEC'] as const;

/** "29 OCT 2026" from numeric date parts: three-letter months keep the date column tabular (ICU says "Sept"). */
export function fmtDate(iso: string): string {
  const d = new Date(iso);
  return `${String(d.getDate()).padStart(2, '0')} ${MONTHS[d.getMonth()]} ${d.getFullYear()}`;
}

function OrdersRecord({ orders, error, onReload, openExternal }: { orders: Order[] | null; error: string | null; onReload: () => void; openExternal: (u: string) => void }) {
  const now = useNow(30_000);
  const scroller = useLenis<HTMLDivElement>();
  if (orders === null) return <RecordMessage title={error ? 'Orders unavailable' : 'Reading your orders'} body={error ?? 'One moment.'} onReload={error ? onReload : undefined} />;
  if (orders.length === 0) return <RecordMessage title="No orders yet" body="Everything you buy is recorded here with its state: paid, delivered, refunded." />;
  return (
    <section className="record panel" aria-label="Order history">
      <h2 className="panel__title">Orders</h2>
      <div className="record__head"><span>Date</span><span>Order</span><span>Item</span><span>Total</span><span>State</span></div>
      <div className="record__rows" ref={scroller}>
        {orders.map((o) => {
          const st = stampFor(o, now);
          return (
            <article key={o.id} className={`record__row record__row--${st.tone}`}>
              <span className="record__date">{fmtDate(o.createdAt)}</span>
              <Code>{o.id}</Code>
              <span className="record__item">{o.items.map((i) => (i.quantity > 1 ? `${i.quantity} × ${i.name}` : i.name)).join(', ')}</span>
              <span className="record__total">{formatMoney(o.totalMinor, o.currency)}</span>
              <span className="record__stamp">
                <Chip tone={chipTone(st.tone)}>{st.label}</Chip>
                {o.status === 'pending' && o.checkoutUrl && st.label !== 'EXPIRED' && (
                  <button type="button" className="text-link" onClick={() => openExternal(o.checkoutUrl!)}>Pay <Icon name="external" /></button>
                )}
                {o.failureReason && <span className="record__why">{o.failureReason}</span>}
              </span>
            </article>
          );
        })}
      </div>
    </section>
  );
}

/**
 * Beside the order record: what the ledger adds up to. Every figure comes from the API
 * (wallet balances, the VIP window read from owned items, orders paid and delivered).
 */
function LedgerSummary({ orders, items, balances, demo }: { orders: Order[] | null; items: OwnedItem[] | null; balances: Balance[] | null; demo: boolean }) {
  const now = useNow(60_000);
  const spent = orders ? spentByCurrency(orders) : null;
  const paid = orders ? orders.filter((o) => o.status === 'paid' || o.status === 'fulfilled').length : 0;
  return (
    <aside className="summary panel" aria-label="Account summary">
      <div className="summary__block">
        <span className="summary__label">Wallet {demo && <Chip tone="demo">Demo</Chip>}</span>
        {balances === null ? <span className="summary__dim">Reading…</span> : balances.length === 0 ? <span className="summary__dim">Empty. Coins you buy land here.</span> : balances.map((b) => (
          <span key={b.currency} className="summary__count"><span className="summary__n">{fmtBalance(b.balance)}</span> <Code>{b.currency}</Code></span>
        ))}
      </div>
      <div className="summary__block">
        <VipBar vip={items ? vipWindow(items, now) : null} />
      </div>
      <div className="summary__block">
        <span className="summary__label">Spent</span>
        {spent === null ? <span className="summary__dim">Reading…</span> : spent.length === 0 ? <span className="summary__dim">Nothing paid yet.</span> : spent.map((s) => (
          <span key={s.currency} className="summary__money">{formatMoney(s.totalMinor, s.currency)}</span>
        ))}
        {orders !== null && <span className="summary__fine">{paid} paid of {orders.length} orders. Refunds and open orders are not counted.</span>}
      </div>
    </aside>
  );
}

function OwnedRecord({ items, products, error, onShowroom }: { items: OwnedItem[] | null; products: Product[]; error: string | null; onShowroom: () => void }) {
  const scroller = useLenis<HTMLDivElement>();
  if (items === null) return <RecordMessage title={error ? 'Unavailable' : 'Reading what you own'} body={error ?? 'One moment.'} />;
  if (items.length === 0) return <RecordMessage title="Nothing owned yet" body="Items, VIP time and cosmetics you buy show up here once they are delivered to your account." />;
  return (
    <section className="record record--owned panel" aria-label="Owned items">
      <h2 className="panel__title">Owned <span className="servers__count">{items.length}</span></h2>
      <div className="record__rows owned-grid" ref={scroller}>
        {items.map((it) => {
          const product = products.find((p) => p.sku === it.sku);
          return (
            <article key={it.id} className="owned">
              <img className="owned__img" src={thumbUrl(product ? productStill(product) : 'coins')} alt="" decoding="async" />
              <div className="owned__body">
                <span className="owned__name">{product?.name ?? it.sku}</span>
                <span className="owned__kind">{product ? kindLabel(product.kind, product) : it.type}</span>
                <span className="owned__meta">
                  <Chip tone={it.delivered ? 'done' : 'pending'}>{it.delivered ? 'DELIVERED' : 'QUEUED'}</Chip>
                  <span className="record__date">{fmtDate(it.createdAt)}</span>
                </span>
              </div>
            </article>
          );
        })}
        {/* The next shelf slot points back to the showroom instead of standing empty. */}
        <article className="owned owned--more">
          <span className="owned__name">More in the showroom</span>
          <span className="owned__line">Vehicles, outfits, VIP time and coins.</span>
          <Plate tone="ghost" onClick={onShowroom}>Showroom <Icon name="arrow" /></Plate>
        </article>
      </div>
    </section>
  );
}

function RecordMessage({ title, body, onReload }: { title: string; body: string; onReload?: (() => void) | undefined }) {
  return (
    <section className="record record--empty panel">
      <h2 className="record__title">{title}</h2>
      <p className="record__body">{body}</p>
      {onReload && <Plate tone="ghost" onClick={onReload}><Icon name="refresh" /> Try again</Plate>}
    </section>
  );
}
