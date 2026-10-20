export function slugify(value: string): string {
  return normalise(value);
}

function normalise(value: string): string {
  return value.trim().toLowerCase();
}
