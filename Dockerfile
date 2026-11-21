# pg_achievements on top of the official postgres image: a server with the
# extension preloaded and created in the "postgres" database, to try it
# out in one command:
#
#   docker build -t pg_achievements .
#   docker run -d --name pga -e POSTGRES_PASSWORD=secret pg_achievements
#   docker exec -it pga psql -U postgres
#
# Pick another PostgreSQL major version with --build-arg PG_MAJOR=17.

ARG PG_MAJOR=18

# Build stage: compiler and server headers stay out of the final image.
FROM postgres:${PG_MAJOR} AS build
ARG PG_MAJOR
# libkrb5-dev and libssl-dev: libpq-be.h includes the GSSAPI and OpenSSL
# headers on the PGDG builds
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential postgresql-server-dev-${PG_MAJOR} libkrb5-dev libssl-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
ENV PG_CONFIG=/usr/lib/postgresql/${PG_MAJOR}/bin/pg_config
RUN make PG_CONFIG=$PG_CONFIG \
 && make PG_CONFIG=$PG_CONFIG install DESTDIR=/out

FROM postgres:${PG_MAJOR}
COPY --from=build /out/ /
COPY docker/initdb.sh /docker-entrypoint-initdb.d/10-pg_achievements.sh
# The entrypoint passes these arguments to the temporary server that runs
# the init scripts too, so CREATE EXTENSION finds the library preloaded.
CMD ["postgres", "-c", "shared_preload_libraries=pg_achievements"]
