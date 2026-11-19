// Fibonacci128: Evil Genius Labs' 128 lights on a golden-angle spiral, in the order the strand is wired.
// The board of jasoncoon/fibonacci128-touch-demo (GPL-3.0): light n sits n golden angles round and
// sqrt(n) out, within 4 units in 256 of the sketch's measured map, and the strand runs out and back
// along the 21 spiral arms. The f128 effects beside it find light n by the same arithmetic.

class F128Layout {
  byte size = 32;

  int dimensions() { return 2; }

  string tags() { return "💫"; }

  void defineControls() {
    addControl("size", size, 20, 128); // the square the board spans; 20 is the smallest that gives every light a cell of its own
  }

  // The integer square root, by halving steps: the radius of light n is sqrt(128 n + 64).
  int isqrt(int v) {
    int r = 0;
    int s = 128;
    for (int k = 0; k < 8; k = k + 1) {
      if ((r + s) * (r + s) <= v) { r = r + s; }
      s = s / 2;
    }
    return r;
  }

  // Light n on the sketch's 0..255 board: n golden angles (25033 of 65536) round, its radius out from 128.
  // The spiral's box there is 2..252 by 3..249, mapped edge to edge onto the square, so the effects can
  // take its side from the grid's height and a light added beside the board leaves them in step.
  void place(int n) {
    int r = isqrt(128 * n + 64);
    int bx = 128 + (cos(n * 25033) - 32768) * r / 32768;
    int by = 128 + (sin(n * 25033) - 32768) * r / 32768;
    addLight((bx - 2) * (size - 1) / 250, (by - 3) * (size - 1) / 246, 0);
  }

  // Arm k of the strand holds lights a, a + 21, a + 42, up to 127, with a = 8k mod 21: out along the
  // even arms, back along the odd ones.
  void placeLights() {
    for (int k = 0; k < 21; k = k + 1) {
      int a = (k * 8) % 21;
      int top = a + (127 - a) / 21 * 21;
      for (int m = a; m <= top; m = m + 21) {
        if (k % 2 == 0) { place(m); }
        else { place(top + a - m); }
      }
    }
  }
}
