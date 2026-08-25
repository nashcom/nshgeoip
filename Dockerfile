# Static Alpine/musl build of nshgeoip, packaged into a "FROM scratch"
# runtime image -- no base OS at all, just the static binary. Alpine
# packages a static (.a) build of libmaxminddb separately from its
# shared-lib package -- apk add libmaxminddb-static pulls that in directly,
# tracked and updated by Alpine's own package maintenance, so there's no
# need to build it from an upstream release tarball ourselves.
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

RUN mkdir /image-tmp

FROM scratch AS runtime

COPY --from=build /src/nshgeoip /nshgeoip
COPY --from=build --chmod=1777 /image-tmp /tmp
COPY --from=build --chown=1000:1000 --chmod=0755 /image-tmp /run

USER 1000:1000

ENTRYPOINT ["/nshgeoip"]

HEALTHCHECK CMD ["/nshgeoip", "--health-check"]
