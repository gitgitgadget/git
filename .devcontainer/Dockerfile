FROM ubuntu:latest

ARG USER_ID
ARG GROUP_ID

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Europe/Copenhagen

RUN apt-get update \
    && \
    apt-get install -y \
        sudo \
        build-essential \
        libcurl4-gnutls-dev \
        libexpat1-dev \
        gettext \
        libz-dev \
        libssl-dev \
        asciidoc \
        xmlto \
        docbook-xsl \
        \
        tzdata \
        git \
        coccinelle \
    && \
    ln -fs /usr/share/zoneinfo/$TZ /etc/localtime \
    && \
    dpkg-reconfigure --frontend noninteractive tzdata

RUN  apt-get update && apt-get install -y autoconf

RUN addgroup -gid 1001 gituser
RUN adduser --disabled-password -u 1001 -gid 1001 gituser
RUN usermod -aG sudo gituser
RUN echo 'gituser ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/010-gituser
RUN chmod 0440 /etc/sudoers.d/010-gituser

USER 1001

#WORKDIR /home/gituser/git-src/

#RUN  make configure
#RUN ./configure --prefix=${HOME}/.local/

#RUN make -j$(nproc) gitweb || make gitweb 
#RUN make install-gitweb

#RUN make -j$(nproc) || make
#RUN make -j$(nproc) gitweb || make gitweb 
#RUN make install-gitweb
#RUN make -j$(nproc) NO_PERL=YesPlease install || make NO_PERL=YesPlease install
#RUN make install


RUN mkdir -p  ${HOME}/.local/
#RUN ls -la ${HOME}/.local/bin
#ENV PATH="/home/gituser/.local/bin:${PATH}"
#RUN ls -l $HOME/.local/bin
#RUN echo $PATH $HOME 

#RUN git --version
#RUN which git 

RUN git config --global user.email "gituser@example.com"
RUN git config --global user.name "Git User"

#WORKDIR /home/gituser/git-test/

#CMD [ "git" ]
