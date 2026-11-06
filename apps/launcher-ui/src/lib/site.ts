// The Solar RP website, for the pages the launcher hands off to the system browser (bridge
// `openExternal`, https only). One constant on purpose: the platform API base (`settings.apiBase`)
// does not say where the website lives (it may be a subdomain, or loopback while testing), so the
// site origin is not derived from it.

export const SITE_ORIGIN = 'https://solar-rp.com';

/** Account creation happens on the website only; the launcher logs in. */
export const SIGNUP_URL = `${SITE_ORIGIN}/signup/`;

/** The account page: linking Discord (the quests that pay coins need it). */
export const ACCOUNT_URL = `${SITE_ORIGIN}/account/`;
