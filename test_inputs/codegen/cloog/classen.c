for (int c0 = 0; c0 < 2 * m - 1; c0 += 1) {
  if (2 * m >= c0 + 3 && c0 >= 1) {
    if (c0 + 1 == m) {
      S5(m - 2, 1, m - 1, 1, m - 1, 1, m, 1);
      S1(m - 1, 1, m, 1);
      S3(m - 1, 1, m, 2, m, 1, m, 2);
    } else if (c0 >= m) {
      S5(c0 - 1, -m + c0 + 2, c0, -m + c0 + 2, m - 1, -m + c0 + 2, m, -m + c0 + 2);
      S6(c0 - 1, -m + c0 + 1, c0, -m + c0 + 2, m, -m + c0 + 1, m, -m + c0 + 2);
      S1(c0, -m + c0 + 2, m, -m + c0 + 2);
      S3(c0, -m + c0 + 2, c0 + 1, -m + c0 + 3, m, -m + c0 + 2, m, -m + c0 + 3);
    } else {
      S5(c0 - 1, 1, c0, 1, c0, 1, c0 + 1, 1);
      S1(c0, 1, c0 + 1, 1);
      S3(c0, 1, c0 + 1, 2, c0 + 1, 1, c0 + 1, 2);
      S4(c0, 1, c0 + 2, 2, c0 + 1, 1, c0 + 2, 2);
      S2(c0, 1, c0 + 1, 1, c0 + 1, 1, c0 + 2, 1);
    }
    for (int c2 = max(2, -m + c0 + 3); c2 <= min(m - 1, c0); c2 += 1) {
      S5(c0 - 1, c2, c0, c2, c0 - c2 + 1, c2, c0 - c2 + 2, c2);
      S7(c0 - 1, c2 - 1, c0 + 1, c2, c0 - c2 + 2, c2 - 1, c0 - c2 + 3, c2);
      S6(c0 - 1, c2 - 1, c0, c2, c0 - c2 + 2, c2 - 1, c0 - c2 + 2, c2);
      S1(c0, c2, c0 - c2 + 2, c2);
      S3(c0, c2, c0 + 1, c2 + 1, c0 - c2 + 2, c2, c0 - c2 + 2, c2 + 1);
      S4(c0, c2, c0 + 2, c2 + 1, c0 - c2 + 2, c2, c0 - c2 + 3, c2 + 1);
      S2(c0, c2, c0 + 1, c2, c0 - c2 + 2, c2, c0 - c2 + 3, c2);
    }
    if (c0 + 1 == m) {
      S7(m - 2, m - 1, m, m, 1, m - 1, 2, m);
      S6(m - 2, m - 1, m - 1, m, 1, m - 1, 1, m);
      S1(m - 1, m, 1, m);
      S2(m - 1, m, m, m, 1, m, 2, m);
    } else if (c0 >= m) {
      S5(c0 - 1, m, c0, m, -m + c0 + 1, m, -m + c0 + 2, m);
      S7(c0 - 1, m - 1, c0 + 1, m, -m + c0 + 2, m - 1, -m + c0 + 3, m);
      S6(c0 - 1, m - 1, c0, m, -m + c0 + 2, m - 1, -m + c0 + 2, m);
      S1(c0, m, -m + c0 + 2, m);
      S2(c0, m, c0 + 1, m, -m + c0 + 2, m, -m + c0 + 3, m);
    } else {
      S7(c0 - 1, c0, c0 + 1, c0 + 1, 1, c0, 2, c0 + 1);
      S6(c0 - 1, c0, c0, c0 + 1, 1, c0, 1, c0 + 1);
      S1(c0, c0 + 1, 1, c0 + 1);
      S3(c0, c0 + 1, c0 + 1, c0 + 2, 1, c0 + 1, 1, c0 + 2);
      S4(c0, c0 + 1, c0 + 2, c0 + 2, 1, c0 + 1, 2, c0 + 2);
      S2(c0, c0 + 1, c0 + 1, c0 + 1, 1, c0 + 1, 2, c0 + 1);
    }
  } else if (m >= 2 && c0 == 0) {
    S1(0, 1, 1, 1);
    S3(0, 1, 1, 2, 1, 1, 1, 2);
    S4(0, 1, 2, 2, 1, 1, 2, 2);
    S2(0, 1, 1, 1, 1, 1, 2, 1);
  } else if (m >= 2) {
    S5(2 * m - 3, m, 2 * m - 2, m, m - 1, m, m, m);
    S6(2 * m - 3, m - 1, 2 * m - 2, m, m, m - 1, m, m);
    S1(2 * m - 2, m, m, m);
  } else
    S1(0, 1, 1, 1);
  for (int c8 = max(1, -m + c0 + 2); c8 <= min(m, c0 + 1); c8 += 1)
    S8(c0, c8);
}
