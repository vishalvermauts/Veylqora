import { check } from '../auth';
import { TokenStore } from '../auth/tokens';
import * as text from '../util/text';

export function handle(token: string): string {
  if (check(token)) {
    new TokenStore().save(token);
  }
  return text.slugify(token);
}
