/*
 *  Licensed to the Apache Software Foundation (ASF) under one or more
 *  contributor license agreements.  See the NOTICE file distributed with
 *  this work for additional information regarding copyright ownership.
 *  The ASF licenses this file to You under the Apache License, Version 2.0
 *  (the "License"); you may not use this file except in compliance with
 *  the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

package java.net;

import java.io.UnsupportedEncodingException;
import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.nio.charset.Charset;
import java.nio.charset.IllegalCharsetNameException;
import java.nio.charset.UnsupportedCharsetException;

/**
 * This class is used to decode a string which is encoded in the
 * {@code application/x-www-form-urlencoded} MIME content type.
 */
public class URLDecoder {
	/**
	 * Decodes the argument which is assumed to be encoded in the
	 * {@code x-www-form-urlencoded} MIME content type.
	 * <p>
	 * '+' will be converted to space, '%' and two following hex digit
	 * characters are converted to the equivalent byte value. All other
	 * characters are passed through unmodified. For example "A+B+C %24%25" ->
	 * "A B C $%".
	 * 
	 * @param s
	 *            the encoded string.
	 * @return the decoded clear-text representation of the given string.
	 * @deprecated use {@link #decode(String, String)} instead.
	 */
	@Deprecated
	public static String decode(String s) {
		return decode(s, Charset.defaultCharset());
	}

	/**
	 * Decodes the argument which is assumed to be encoded in the
	 * {@code x-www-form-urlencoded} MIME content type using the specified
	 * encoding scheme.
	 * <p>
	 * '+' will be converted to space, '%' and two following hex digit
	 * characters are converted to the equivalent byte value. All other
	 * characters are passed through unmodified. For example "A+B+C %24%25" ->
	 * "A B C $%".
	 * 
	 * @param s
	 *            the encoded string.
	 * @param encoding
	 *            the encoding scheme to be used.
	 * @return the decoded clear-text representation of the given string.
	 * @throws UnsupportedEncodingException
	 *             if the specified encoding scheme is invalid.
	 */
	public static String decode(String s, String encoding)
			throws UnsupportedEncodingException {
		if (encoding == null) {
			throw new NullPointerException();
		}
		if (encoding.isEmpty()) {
			throw new UnsupportedEncodingException(encoding);
		}

		if (s.indexOf('%') == -1) {
			if (s.indexOf('+') == -1)
				return s;
			char[] str = s.toCharArray();
			for (int i = 0; i < str.length; i++) {
				if (str[i] == '+')
					str[i] = ' ';
			}
			return new String(str);
		}

		Charset charset = null;
		try {
			charset = Charset.forName(encoding);
		} catch (IllegalCharsetNameException e) {
			throw (UnsupportedEncodingException) (new UnsupportedEncodingException(
					encoding).initCause(e));
		} catch (UnsupportedCharsetException e) {
			throw (UnsupportedEncodingException) (new UnsupportedEncodingException(
					encoding).initCause(e));
		}

		return decode(s, charset);
	}

	private static String decode(String s, Charset charset) {

		char[] str_buf = new char[s.length()];
		byte[] buf = new byte[s.length() / 3];
		int buf_len = 0;

		for (int i = 0; i < s.length();) {
			char c = s.charAt(i);
			if (c == '+') {
				str_buf[buf_len] = ' ';
			} else if (c == '%') {

				int len = 0;
				do {
					if (i + 2 >= s.length()) {
						throw new IllegalArgumentException(
								"Incomplete % sequence at: " + i);
					}
					int d1 = Character.digit(s.charAt(i + 1), 16);
					int d2 = Character.digit(s.charAt(i + 2), 16);
					if (d1 == -1 || d2 == -1) {
						throw new IllegalArgumentException(
								"Invalid % sequence " + s.substring(i, i + 3)
										+ " at " + i);
					}
					buf[len++] = (byte) ((d1 << 4) + d2);
					i += 3;
				} while (i < s.length() && s.charAt(i) == '%');

				CharBuffer cb = charset.decode(ByteBuffer.wrap(buf, 0, len));
				len = cb.length();
				System.arraycopy(cb.array(), 0, str_buf, buf_len, len);
				buf_len += len;
				continue;
			} else {
				str_buf[buf_len] = c;
			}
			i++;
			buf_len++;
		}
		return new String(str_buf, 0, buf_len);
	}
}
