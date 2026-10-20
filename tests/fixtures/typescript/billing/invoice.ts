import { BaseInvoice } from './base';

export class Invoice extends BaseInvoice {
  amount(): number {
    return this.total();
  }
}
