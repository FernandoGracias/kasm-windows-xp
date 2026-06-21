FROM kasmweb/core-ubuntu-noble:1.18.0-rolling-daily
USER root

ENV HOME=/home/kasm-default-profile
ENV STARTUPDIR=/dockerstartup
WORKDIR $HOME

RUN apt-get update && apt-get install -y --no-install-recommends \
    qemu-system-x86 \
    qemu-system-gui \
    qemu-utils \
    xclip \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/winxp
COPY winxp.qcow2 /opt/winxp/winxp.qcow2
COPY host/xp_bridge_daemon.py /opt/winxp/xp_bridge_daemon.py
COPY startup.sh /opt/winxp/startup.sh
RUN chmod +x /opt/winxp/startup.sh

COPY custom_startup.sh $STARTUPDIR/custom_startup.sh
RUN chmod +x $STARTUPDIR/custom_startup.sh

RUN chown 1000:0 $HOME
RUN $STARTUPDIR/set_user_permission.sh $HOME

ENV HOME=/home/kasm-user
WORKDIR $HOME
RUN mkdir -p $HOME && chown -R 1000:0 $HOME
USER 1000
