# SSH jump host for the tunnelled integration tests (compose.yaml, service
# sshd). A file rather than compose's dockerfile_inline, which podman's build
# path does not take.
FROM alpine:3.20
RUN apk add --no-cache openssh \
 && ssh-keygen -A \
 && adduser -D tunnel \
 && echo 'tunnel:tunnelpw' | chpasswd \
 && sed -i -e 's/^#\?PasswordAuthentication.*/PasswordAuthentication yes/' \
           -e 's/^#\?AllowTcpForwarding.*/AllowTcpForwarding yes/' \
           /etc/ssh/sshd_config
CMD ["/usr/sbin/sshd", "-D", "-e"]
