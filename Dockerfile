# Static Alpine/musl build of nshgeoip. Alpine packages a static (.a) build
# of libmaxminddb separately from its shared-lib package -- apk add
# libmaxminddb-static pulls that in directly, tracked and updated by
# Alpine's own package maintenance, so there's no need to build it from
# an upstream release tarball ourselves. The runtime stage below needs
# nothing from Alpine's package repo at all: the binary is fully static.
#
# The actual compile/link steps live in docker/compile_alpine_static.sh,
# not inline here -- see that script's own comment for why (fortify-shim
# workaround, easier to iterate on/test standalone).
ARG ALPINE_VERSION=latest

FROM alpine:${ALPINE_VERSION} AS build
RUN apk add --no-cache g++ make file libmaxminddb-dev libmaxminddb-static

WORKDIR /src
COPY Makefile ./
COPY src ./src
COPY docker ./docker
RUN ./docker/compile_alpine_static.sh

FROM alpine:${ALPINE_VERSION} AS runtime
RUN addgroup -S nshgeoip && adduser -S -D -H -G nshgeoip nshgeoip
COPY --from=build /src/nshgeoip /usr/local/sbin/nshgeoip
USER nshgeoip
ENTRYPOINT ["/usr/local/sbin/nshgeoip"]
CMD ["--config", "/etc/nshgeoip/nshgeoip.conf"]
