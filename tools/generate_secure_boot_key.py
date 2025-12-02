#!/usr/bin/env python3
"""
Generate RSA-3072 key pair for ESP32-C3 Secure Boot v2.

This script generates the signing key needed for firmware signature verification.
"""

import os
import sys

try:
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.backends import default_backend
except ImportError:
    print("Error: cryptography module not installed")
    print("Install with: pip install cryptography")
    sys.exit(1)


def generate_secure_boot_key(output_file="secure_boot_signing_key.pem"):
    """Generate RSA-3072 key for ESP32-C3 Secure Boot v2"""
    
    print(f"Generating RSA-3072 key pair for ESP32-C3 Secure Boot v2...")
    print(f"This may take a minute...")
    
    # Generate RSA-3072 private key
    private_key = rsa.generate_private_key(
        public_exponent=65537,
        key_size=3072,
        backend=default_backend()
    )
    
    # Serialize private key to PEM format
    pem = private_key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption()
    )
    
    # Write to file
    with open(output_file, 'wb') as f:
        f.write(pem)
    
    # Set restrictive permissions (read-only for owner)
    os.chmod(output_file, 0o400)
    
    print(f"✓ Private key generated: {output_file}")
    print(f"✓ Key size: 3072-bit RSA")
    print(f"✓ Format: PEM (PKCS8)")
    print()
    print("WARNING: Keep this key secure!")
    print("This key is used to sign all firmware updates.")
    print()
    
    # Also save public key for reference
    public_key = private_key.public_key()
    public_pem = public_key.public_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PublicFormat.SubjectPublicKeyInfo
    )
    
    public_file = output_file.replace(".pem", "_public.pem")
    with open(public_file, 'wb') as f:
        f.write(public_pem)
    
    print(f"✓ Public key saved: {public_file}")
    print()
    print("Next steps:")
    print(f"1. Keep {output_file} secure (never share)")
    print(f"2. Use with idf.py to sign firmware:")
    print(f"   idf.py -p COM9 secure-boot-sign-binaries --keyfile {output_file}")
    print(f"3. Enable CONFIG_SECURE_BOOT in menuconfig")
    print(f"4. Flash with: idf.py -p COM9 flash")


if __name__ == "__main__":
    output_file = "secure_boot_signing_key.pem"
    if len(sys.argv) > 1:
        output_file = sys.argv[1]
    
    if os.path.exists(output_file):
        print(f"Error: {output_file} already exists")
        print("Remove it first if you want to generate a new key")
        sys.exit(1)
    
    generate_secure_boot_key(output_file)
