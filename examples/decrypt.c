/* libp11 example code: auth.c
 *
 * This examply simply connects to your smart card
 * and does a public key authentication.
 *
 * Feel free to copy all of the code as needed.
 *
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <stdio.h>
#include <string.h>
#include <libp11.h>

#define RANDOM_SOURCE "/dev/urandom"
#define RANDOM_SIZE 64
#define MAX_SIGSIZE 256

int main(int argc, char *argv[])
{
	PKCS11_CTX *ctx;
	PKCS11_SLOT *slots, *slot;
	PKCS11_CERT *certs;
	
	PKCS11_KEY *authkey;
	PKCS11_CERT *authcert;
	EVP_PKEY *pubkey = NULL;

	unsigned char *random = NULL, *encrypted = NULL, *decrypted = NULL;

	char password[20];
	int rc = 0, fd, len;
	unsigned int nslots, ncerts;

	/* get password */
	struct termios old, new;

	if (argc != 2) {
		fprintf(stderr, "usage: auth /usr/lib/opensc-pkcs11.so\n");
		return 1;
	}

	ctx = PKCS11_CTX_new();

	/* load pkcs #11 module */
	rc = PKCS11_CTX_load(ctx, argv[1]);
	if (rc) {
		fprintf(stderr, "loading pkcs11 engine failed: %s\n",
			ERR_reason_error_string(ERR_get_error()));
		rc = 1;
		goto nolib;
	}

	/* get information on all slots */
	rc = PKCS11_enumerate_slots(ctx, &slots, &nslots);
	if (rc < 0) {
		fprintf(stderr, "no slots available\n");
		rc = 2;
		goto noslots;
	}

	/* get first slot with a token */
	slot = PKCS11_find_token(ctx, slots, nslots);
	if (slot == NULL || slot->token == NULL) {
		fprintf(stderr, "no token available\n");
		rc = 3;
		goto notoken;
	}
	printf("Slot manufacturer......: %s\n", slot->manufacturer);
	printf("Slot description.......: %s\n", slot->description);
	printf("Slot token label.......: %s\n", slot->token->label);
	printf("Slot token manufacturer: %s\n", slot->token->manufacturer);
	printf("Slot token model.......: %s\n", slot->token->model);
	printf("Slot token serialnr....: %s\n", slot->token->serialnr);

	/* get all certs */
	rc = PKCS11_enumerate_certs(slot->token, &certs, &ncerts);
	if (rc) {
		fprintf(stderr, "PKCS11_enumerate_certs failed\n");
		goto failed;
	}
	if (ncerts <= 0) {
		fprintf(stderr, "no certificates found\n");
		goto failed;
	}

	/* use the first cert */
	authcert=&certs[0];

	/* get random bytes */
	random = OPENSSL_malloc(RANDOM_SIZE);
	if (random == NULL)
		goto failed;

	fd = open(RANDOM_SOURCE, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "fatal: cannot open RANDOM_SOURCE: %s\n",
				strerror(errno));
		goto failed;
	}

	rc = read(fd, random, RANDOM_SIZE);
	if (rc < 0) {
		fprintf(stderr, "fatal: read from random source failed: %s\n",
			strerror(errno));
		close(fd);
		goto failed;
	}

	if (rc < RANDOM_SIZE) {
		fprintf(stderr, "fatal: read returned less than %d<%d bytes\n",
			rc, RANDOM_SIZE);
		close(fd);
		goto failed;
	}

	close(fd);

	/* get RSA key */
	pubkey = X509_get_pubkey(authcert->x509);
	if (pubkey == NULL) {
		fprintf(stderr, "could not extract public key\n");
		goto failed;
	}

	/* allocate destination buffer */
#if OPENSSL_VERSION_NUMBER >= 0x10100003L
	encrypted = OPENSSL_malloc(RSA_size(EVP_PKEY_get0_RSA(pubkey)));
#else
	encrypted = OPENSSL_malloc(RSA_size(pubkey->pkey.rsa));
#endif
	if (encrypted == NULL) {
		fprintf(stderr,"out of memory for encrypted data");
		goto failed;
	}

	/* use public key for encryption */
	len = RSA_public_encrypt(RANDOM_SIZE, random, encrypted,
#if OPENSSL_VERSION_NUMBER >= 0x10100003L
			EVP_PKEY_get0_RSA(pubkey),
#else
			pubkey->pkey.rsa,
#endif
			RSA_PKCS1_PADDING);
	if (len < 0) {
		fprintf(stderr, "fatal: RSA_public_encrypt failed\n");
		goto failed;
	}

	/* now decrypt */
	if (!slot->token->loginRequired)
		goto loggedin;

	/* Turn echoing off and fail if we can't. */
	if (tcgetattr(0, &old) != 0)
		goto failed;

	new = old;
	new.c_lflag &= ~ECHO;
	if (tcsetattr(0, TCSAFLUSH, &new) != 0)
		goto failed;

	/* Read the password. */
	printf("Password for token %.32s: ", slot->token->label);
	if (fgets(password, sizeof(password), stdin) == NULL)
		goto failed;

	/* Restore terminal. */
	(void)tcsetattr(0, TCSAFLUSH, &old);

	/* strip tailing \n from password */
	rc = strlen(password);
	if (rc <= 0)
		goto failed;
	password[rc-1]=0;

	/* perform pkcs #11 login */
	rc = PKCS11_login(slot, 0, password);
	memset(password, 0, strlen(password));
	if (rc != 0) {
		fprintf(stderr, "PKCS11_login failed\n");
		goto failed;
	}

loggedin:

	authkey = PKCS11_find_key(authcert);
	if (authkey == NULL) {
		fprintf(stderr, "no key matching certificate available\n");
		goto failed;
	}

	/* allocate space for decrypted data */
#if OPENSSL_VERSION_NUMBER >= 0x10100003L
	decrypted = OPENSSL_malloc(RSA_size(EVP_PKEY_get0_RSA(pubkey)));
#else
	decrypted = OPENSSL_malloc(RSA_size(pubkey->pkey.rsa));
#endif
	if (decrypted == NULL)
		goto failed;

	rc = PKCS11_private_decrypt(len, encrypted,
			decrypted, authkey, RSA_PKCS1_PADDING);
	if (rc != RANDOM_SIZE) {
		fprintf(stderr, "fatal: PKCS11_private_decrypt failed\n");
		goto failed;
	}

	/* check if original matches decypted */
	if (memcmp(random, decrypted, RANDOM_SIZE) != 0) {
		fprintf(stderr, "fatal: decrypted data does not match original\n");
		goto failed;
	}

	PKCS11_release_all_slots(ctx, slots, nslots);
	PKCS11_CTX_unload(ctx);
	PKCS11_CTX_free(ctx);

	if (pubkey != NULL)
		EVP_PKEY_free(pubkey);
	if (random != NULL)
		OPENSSL_free(random);
	if (encrypted != NULL)
		OPENSSL_free(encrypted);
	if (decrypted != NULL)
		OPENSSL_free(decrypted);

	CRYPTO_cleanup_all_ex_data();
	ERR_free_strings();
#if OPENSSL_VERSION_NUMBER >= 0x10100006L
	/* OpenSSL version >= 1.1.0-pre6 */
	/* the function is no longer needed */
#elif OPENSSL_VERSION_NUMBER >= 0x10100004L
	/* OpenSSL version 1.1.0-pre4 or 1.1.0-pre5 */
	ERR_remove_thread_state();
#elif OPENSSL_VERSION_NUMBER >= 0x10000000L
	/* OpenSSL version >= 1.0.0 */
	ERR_remove_thread_state(NULL);
#else
	/* OpenSSL version < 1.0.0 */
	ERR_remove_state(0);
#endif

	printf("decryption successfull.\n");
	return 0;


failed:
	ERR_print_errors_fp(stderr); 
notoken:
	PKCS11_release_all_slots(ctx, slots, nslots);

noslots:
	PKCS11_CTX_unload(ctx);

nolib:
	PKCS11_CTX_free(ctx);
	

	printf("decryption failed.\n");
	return 1;
}

/* vim: set noexpandtab: */
