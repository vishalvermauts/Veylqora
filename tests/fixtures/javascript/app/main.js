import { Square } from '../lib/geometry.js';
import * as geo from '../lib/geometry';

export function run() {
  const s = new Square();
  return geo.area(1, 2) + s.side();
}
