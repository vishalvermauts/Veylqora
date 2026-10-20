export function area(width, height) {
  return multiply(width, height);
}

function multiply(a, b) {
  return a * b;
}

export class Shape {
  describe() {
    return this.label();
  }

  label() {
    return 'shape';
  }
}

export class Square extends Shape {
  side() {
    return area(this.s, this.s);
  }
}
