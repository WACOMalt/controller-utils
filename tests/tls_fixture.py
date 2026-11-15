import subprocess


def certificate(directory, domains=False):
    public, private = directory / "certificate.pem", directory / "private.pem"
    subprocess.run(
        [
            "openssl",
            "req",
            "-x509",
            "-newkey",
            "rsa:2048",
            "-nodes",
            "-keyout",
            str(private),
            "-out",
            str(public),
            "-subj",
            "/CN=127.0.0.1",
            "-addext",
            "subjectAltName=IP:127.0.0.1"
            + (",DNS:github.com,DNS:release-assets.githubusercontent.com" if domains else ""),
            "-days",
            "1",
        ],
        capture_output=True,
        check=True,
        timeout=10,
    )
    private.chmod(0o600)
    return public, private
