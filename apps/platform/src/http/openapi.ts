import { z } from 'zod';
import { openApiPath, type RouteMeta } from './router.js';

// An OpenAPI 3.1 description generated from the route definitions, so it cannot drift from what
// the server validates: request bodies and query strings are the same zod schemas the handlers
// run, converted with z.toJSONSchema. Responses are prose (docs/api.md has the examples).

function schemaOf(schema: z.ZodType | undefined): Record<string, unknown> | undefined {
  if (!schema) return undefined;
  try {
    const json = z.toJSONSchema(schema, { io: 'input', unrepresentable: 'any' }) as Record<string, unknown>;
    delete json['$schema'];
    return json;
  } catch {
    return {};
  }
}

function pathParams(path: string): { name: string; in: 'path'; required: true; schema: { type: 'string' } }[] {
  return [...path.matchAll(/:([A-Za-z0-9_]+)/g)].map((m) => ({ name: m[1] as string, in: 'path', required: true, schema: { type: 'string' } }));
}

function security(auth: RouteMeta['auth']): Record<string, string[]>[] {
  switch (auth) {
    case 'session':
    case 'mod':
    case 'admin':
      return [{ sessionToken: [] }];
    case 'server':
      return [{ serverId: [], serverSecret: [] }];
    default:
      return [];
  }
}

export function buildOpenApi(routes: readonly RouteMeta[], info: { version: string; serverUrl: string }): Record<string, unknown> {
  const paths: Record<string, Record<string, unknown>> = {};
  for (const r of routes) {
    const key = openApiPath(r.path);
    const entry = (paths[key] ??= {});
    const querySchema = schemaOf(r.query) as { properties?: Record<string, unknown>; required?: string[] } | undefined;
    const parameters: unknown[] = [...pathParams(r.path)];
    for (const [name, schema] of Object.entries(querySchema?.properties ?? {})) {
      parameters.push({ name, in: 'query', required: querySchema?.required?.includes(name) ?? false, schema });
    }
    const body = schemaOf(r.body);
    entry[r.method] = {
      summary: r.summary,
      ...(r.description ? { description: r.description } : {}),
      tags: r.tags,
      ...(parameters.length > 0 ? { parameters } : {}),
      ...(body ? { requestBody: { required: true, content: { 'application/json': { schema: body } } } } : {}),
      security: security(r.auth),
      'x-auth': r.auth,
      'x-surface': r.surface,
      'x-errors': r.errors,
      'x-rate-limits': r.limits.map((l) => `${l.limit} per ${l.windowSeconds}s per ${l.scope} (${l.name})`),
      responses: {
        [String(r.successStatus)]: { description: r.response },
        default: { description: 'Error: { "error": { "code", "message", "requestId", "details"? } }' },
      },
    };
  }
  return {
    openapi: '3.1.0',
    info: { title: 'GAGE-MP platform API', version: info.version, description: 'Accounts, login, join tokens, official server list, shop and settings. Reference with examples: platform/docs/api.md.' },
    servers: [{ url: info.serverUrl }],
    paths,
    components: {
      securitySchemes: {
        sessionToken: { type: 'http', scheme: 'bearer', description: 'Opaque session token from /v1/auth/login (prefix gms_).' },
        serverId: { type: 'apiKey', in: 'header', name: 'X-Gage-Server-Id' },
        serverSecret: { type: 'apiKey', in: 'header', name: 'X-Gage-Server-Secret' },
      },
    },
  };
}
