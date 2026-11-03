import { useEffect, useState } from 'react';
import { z } from 'zod';

const errorSchema = z.object({ error: z.string(), fields: z.record(z.string(), z.array(z.string())).optional() });
export class ApiError extends Error {
  constructor(message: string, readonly status: number, readonly fields: Record<string, string[]> = {}) { super(message); }
}

export async function api<T>(url: string, schema: z.ZodType<T>, options?: RequestInit): Promise<T> {
  const response = await fetch(url, { ...options, credentials: 'same-origin', headers: { ...(options?.body ? { 'Content-Type': 'application/json' } : {}), ...options?.headers } });
  if (!response.ok) {
    const body = errorSchema.safeParse(await response.json().catch(() => null));
    throw new ApiError(body.success ? body.data.error : 'Something went wrong. Please try again.', response.status, body.success ? body.data.fields : undefined);
  }
  return schema.parse(await response.json());
}

type LoadState<T> = { kind: 'loading' } | { kind: 'error'; message: string } | { kind: 'ready'; data: T };
export function useApi<T>(url: string, schema: z.ZodType<T>) {
  const [state, setState] = useState<LoadState<T>>({ kind: 'loading' });
  useEffect(() => {
    const controller = new AbortController();
    setState({ kind: 'loading' });
    void api(url, schema, { signal: controller.signal }).then(data => setState({ kind: 'ready', data })).catch((error: unknown) => {
      if (!controller.signal.aborted) setState({ kind: 'error', message: error instanceof Error ? error.message : 'Could not load this page.' });
    });
    return () => controller.abort();
  }, [url, schema]);
  return state;
}

export function useDebounced(value: string, delay = 250) {
  const [debounced, setDebounced] = useState(value);
  useEffect(() => { const timer = setTimeout(() => setDebounced(value), delay); return () => clearTimeout(timer); }, [value, delay]);
  return debounced;
}
