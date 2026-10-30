package app.screenfleet;

import java.math.BigInteger;
import java.nio.charset.Charset;
import java.security.KeyFactory;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.MessageDigest;
import java.security.PrivateKey;
import java.security.PublicKey;
import java.security.SecureRandom;
import java.security.interfaces.ECPublicKey;
import java.security.spec.ECGenParameterSpec;
import java.security.spec.ECParameterSpec;
import java.security.spec.ECPoint;
import java.security.spec.ECPrivateKeySpec;
import java.security.spec.ECPublicKeySpec;
import java.util.Arrays;

import javax.crypto.Cipher;
import javax.crypto.KeyAgreement;
import javax.crypto.Mac;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * The primitives pairing and encrypted sessions need (protocol/PROTOCOL.md),
 * from the platform's own provider. Used on Android 5+ only (Wi-Fi).
 */
final class Crypto {
    private static final Charset UTF8 = Charset.forName("UTF-8");
    private static final SecureRandom RANDOM = new SecureRandom();
    // P-256 curve: y^2 = x^3 - 3x + b (mod p)
    private static final BigInteger P = new BigInteger("ffffffff00000001000000000000000000000000ffffffffffffffffffffffff", 16);
    private static final BigInteger B = new BigInteger("5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b", 16);

    private Crypto() {}

    static byte[] random(int n) {
        byte[] b = new byte[n];
        RANDOM.nextBytes(b);
        return b;
    }

    static byte[] sha256(byte[]... parts) {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            for (byte[] p : parts) md.update(p);
            return md.digest();
        } catch (Exception e) {
            throw new IllegalStateException(e);
        }
    }

    static byte[] hmac(byte[] key, byte[]... parts) {
        try {
            Mac mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(key, "HmacSHA256"));
            for (byte[] p : parts) mac.update(p);
            return mac.doFinal();
        } catch (Exception e) {
            throw new IllegalStateException(e);
        }
    }

    static byte[] hkdf(byte[] salt, byte[] ikm, String info, int length) {
        return hkdf(salt, ikm, info.getBytes(UTF8), length);
    }

    static byte[] hkdf(byte[] salt, byte[] ikm, byte[] info, int length) {
        byte[] prk = hmac(salt.length == 0 ? new byte[32] : salt, ikm);
        byte[] out = new byte[length];
        byte[] t = new byte[0];
        int pos = 0;
        for (int i = 1; pos < length; ++i) {
            t = hmac(prk, t, info, new byte[] {(byte) i});
            int n = Math.min(t.length, length - pos);
            System.arraycopy(t, 0, out, pos, n);
            pos += n;
        }
        Arrays.fill(prk, (byte) 0);
        return out;
    }

    /** Compares without leaking where the first difference is. */
    static boolean equal(byte[] a, byte[] b) {
        return MessageDigest.isEqual(a, b);
    }

    static byte[] utf8(String s) {
        return s.getBytes(UTF8);
    }

    /** A string as on the wire (u16 length + UTF-8), for transcripts. */
    static byte[] str(String s) {
        byte[] b = utf8(s);
        byte[] out = new byte[2 + b.length];
        out[0] = (byte) (b.length >>> 8);
        out[1] = (byte) b.length;
        System.arraycopy(b, 0, out, 2, b.length);
        return out;
    }

    /** An ECDH P-256 key pair. */
    static final class Ecdh {
        private final PrivateKey privateKey;
        private final ECParameterSpec params;
        final byte[] publicKey; // 04 || X || Y

        Ecdh() throws Exception {
            KeyPairGenerator g = KeyPairGenerator.getInstance("EC");
            g.initialize(new ECGenParameterSpec("secp256r1"), RANDOM);
            KeyPair pair = g.generateKeyPair();
            privateKey = pair.getPrivate();
            ECPublicKey pub = (ECPublicKey) pair.getPublic();
            params = pub.getParams();
            publicKey = encode(pub.getW());
        }

        /** For tests: a known private key. */
        Ecdh(byte[] d, byte[] pub, ECParameterSpec params) throws Exception {
            this.params = params;
            privateKey = KeyFactory.getInstance("EC").generatePrivate(new ECPrivateKeySpec(new BigInteger(1, d), params));
            publicKey = pub.clone();
        }

        ECParameterSpec params() {
            return params;
        }

        /** The shared secret Z (X coordinate, 32 bytes); throws if `peer` isn't a P-256 point. */
        byte[] agree(byte[] peer) throws Exception {
            if (peer.length != 65 || peer[0] != 4) throw new SecurityException("bad public key");
            BigInteger x = new BigInteger(1, Arrays.copyOfRange(peer, 1, 33));
            BigInteger y = new BigInteger(1, Arrays.copyOfRange(peer, 33, 65));
            if (x.compareTo(P) >= 0 || y.compareTo(P) >= 0
                    || !y.multiply(y).mod(P).equals(x.pow(3).subtract(x.multiply(BigInteger.valueOf(3))).add(B).mod(P))) {
                throw new SecurityException("point not on the curve");
            }
            PublicKey key = KeyFactory.getInstance("EC").generatePublic(new ECPublicKeySpec(new ECPoint(x, y), params));
            KeyAgreement ka = KeyAgreement.getInstance("ECDH");
            ka.init(privateKey);
            ka.doPhase(key, true);
            return fixed32(ka.generateSecret());
        }

        private static byte[] encode(ECPoint w) {
            byte[] out = new byte[65];
            out[0] = 4;
            System.arraycopy(fixed32(w.getAffineX().toByteArray()), 0, out, 1, 32);
            System.arraycopy(fixed32(w.getAffineY().toByteArray()), 0, out, 33, 32);
            return out;
        }
    }

    /** Big-endian number bytes as exactly 32 bytes. */
    static byte[] fixed32(byte[] v) {
        if (v.length == 32) return v;
        byte[] out = new byte[32];
        if (v.length > 32) System.arraycopy(v, v.length - 32, out, 0, 32);
        else System.arraycopy(v, 0, out, 32 - v.length, v.length);
        return out;
    }

    /**
     * AES-256-GCM for one direction of a session: the nonce is `direction`
     * followed by a message counter, so it never repeats and is never sent.
     */
    static final class SealKey {
        private final SecretKeySpec key;
        private final int direction;
        private long counter;
        private final Cipher cipher;

        SealKey(byte[] key, int direction) throws Exception {
            this.key = new SecretKeySpec(key, "AES");
            this.direction = direction;
            cipher = Cipher.getInstance("AES/GCM/NoPadding");
        }

        private GCMParameterSpec nonce() {
            if (counter == -1L) throw new SecurityException("counter exhausted");
            byte[] n = new byte[12];
            for (int i = 0; i < 4; ++i) n[i] = (byte) (direction >>> (24 - 8 * i));
            for (int i = 0; i < 8; ++i) n[4 + i] = (byte) (counter >>> (56 - 8 * i));
            ++counter;
            return new GCMParameterSpec(128, n);
        }

        /** ciphertext || 16-byte tag */
        byte[] seal(byte[] plain, int off, int len) throws Exception {
            cipher.init(Cipher.ENCRYPT_MODE, key, nonce());
            return cipher.doFinal(plain, off, len);
        }

        /** Throws if the message was changed. */
        byte[] open(byte[] sealed, int off, int len) throws Exception {
            cipher.init(Cipher.DECRYPT_MODE, key, nonce());
            return cipher.doFinal(sealed, off, len);
        }
    }

    static String hex(byte[] b) {
        StringBuilder s = new StringBuilder();
        for (byte x : b) s.append(String.format("%02x", x & 0xFF));
        return s.toString();
    }

    static byte[] unhex(String s) {
        byte[] out = new byte[s.length() / 2];
        for (int i = 0; i < out.length; ++i) out[i] = (byte) Integer.parseInt(s.substring(2 * i, 2 * i + 2), 16);
        return out;
    }

    /** Known-answer tests, the same as the PC's `sst-host --selftest`. */
    static String selfTest() {
        StringBuilder r = new StringBuilder();
        boolean all = true;
        try {
            all &= check(r, "SHA-256 (FIPS 180-2 \"abc\")", hex(sha256(utf8("abc")))
                    .equals("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
            all &= check(r, "HMAC-SHA256 (RFC 4231 case 2)", hex(hmac(utf8("Jefe"), utf8("what do ya want for nothing?")))
                    .equals("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
            byte[] ikm = new byte[22];
            Arrays.fill(ikm, (byte) 0x0b);
            byte[] okm = hkdf(unhex("000102030405060708090a0b0c"), ikm, unhex("f0f1f2f3f4f5f6f7f8f9"), 42);
            all &= check(r, "HKDF-SHA256 (RFC 5869 case 1)", hex(okm).equals(
                    "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"));

            Ecdh fresh = new Ecdh();
            byte[] mine = unhex("04ead218590119e8876b29146ff89ca61770c4edbbf97d38ce385ed281d8a6b230"
                    + "28af61281fd35e2fa7002523acc85a429cb06ee6648325389f59edfce1405141");
            byte[] theirs = unhex("04700c48f77f56584c5cc632ca65640db91b6bacce3a4df6b42ce7cc838833d287"
                    + "db71e509e3fd9b060ddb20ba5c51dcc5948d46fbf640dfe0441782cab85fa4ac");
            Ecdh known = new Ecdh(unhex("7d7dc5f71eb29ddaf80d6214632eeae03d9058af1fb6d22ed80badb62bc1a534"), mine,
                    fresh.params());
            all &= check(r, "ECDH P-256 (NIST CAVS CDH)", hex(known.agree(theirs))
                    .equals("46fc62106420ff012e54a434fbdd2d25ccc5852060561e68040dd7778997bd7b"));
            Ecdh other = new Ecdh();
            all &= check(r, "ECDH P-256 fresh keys agree", equal(fresh.agree(other.publicKey), other.agree(fresh.publicKey)));
            byte[] bad = theirs.clone();
            bad[64] ^= 1;
            boolean rejected = false;
            try {
                fresh.agree(bad);
            } catch (SecurityException e) {
                rejected = true;
            }
            all &= check(r, "ECDH rejects a point off the curve", rejected);

            all &= check(r, "AES-256-GCM (GCM spec case 13)", hex(new SealKey(new byte[32], 0).seal(new byte[0], 0, 0))
                    .equals("530f8afbc74536b9a963b4f1c4cb738b"));
            all &= check(r, "AES-256-GCM (GCM spec case 14)", hex(new SealKey(new byte[32], 0).seal(new byte[16], 0, 16))
                    .equals("cea7403d4d606b6e074ec5d3baf39d18d0d1c8a799996bf0265b98b5d48ab919"));
            byte[] key = random(32);
            SealKey tx = new SealKey(key, 1), rx = new SealKey(key, 1);
            byte[] msg = utf8("a frame");
            byte[] sealed = tx.seal(msg, 0, msg.length);
            all &= check(r, "AES-256-GCM round trip", Arrays.equals(rx.open(sealed, 0, sealed.length), msg));
            sealed = tx.seal(msg, 0, msg.length);
            sealed[0] ^= 1;
            boolean changed = false;
            try {
                rx.open(sealed, 0, sealed.length);
            } catch (Exception e) {
                changed = true;
            }
            all &= check(r, "AES-256-GCM rejects a changed message", changed);
        } catch (Exception e) {
            r.append("FAIL  ").append(e).append('\n');
            all = false;
        }
        r.append(all ? "all passed" : "FAILED").append('\n');
        return r.toString();
    }

    private static boolean check(StringBuilder r, String name, boolean ok) {
        r.append(ok ? "PASS  " : "FAIL  ").append(name).append('\n');
        return ok;
    }
}
