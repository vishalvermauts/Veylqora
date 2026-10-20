export function checkSignature(token: string): boolean {
  return token.length > 0;
}

export function verifyToken(token: string): boolean {
  return checkSignature(token);
}

export class TokenStore {
  save(token: string): void {
    this.persist(token);
  }

  private persist(token: string): void {
    console.log(token);
  }
}
