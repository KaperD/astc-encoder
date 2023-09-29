FROM mirrors--dockerhub.eu-west-1.artifactory.aws.arm.com/ubuntu:22.04

# Jenkins image build job uses this label to tag version of image
# Update when you make a change
LABEL imageVersion="4.0.0"

RUN useradd -u 1000 -U -m -c Jenkins jenkins

RUN apt update && apt -y upgrade \
  && apt install -y \
    software-properties-common \
    clang \
    gcc \
    g++ \
    git \
    cmake \
    imagemagick \
    make \
    python3 \
    python3-pip \
    python3-venv \
    python3-numpy \
    python3-pil \
    ca-certificates \
    gnupg \
    wget \
  && rm -rf /var/lib/apt/lists/*

# Install python modules
RUN pip3 install requests

# Install Coverity static analysis tools
RUN --mount=type=secret,id=ARTIFACTORY_CREDENTIALS curl -L --user "$(cat /run/secrets/ARTIFACTORY_CREDENTIALS)" "https://eu-west-1.artifactory.aws.arm.com/artifactory/mobile-studio.tools/coverity/cov-analysis-linux64-2023.3.0.sh" --output /tmp/coverity_install.sh &&\
  curl -L --user "$(cat /run/secrets/ARTIFACTORY_CREDENTIALS)" "https://eu-west-1.artifactory.aws.arm.com/artifactory/mobile-studio.tools/coverity/license.dat" --output /tmp/license.dat &&\
  chmod 555 /tmp/coverity_install.sh &&\
  /tmp/coverity_install.sh -q --license.region=6 --license.agreement=agree --license.cov.path=/tmp/license.dat -dir /usr/local/cov-analysis &&\
  rm /tmp/coverity_install.sh /tmp/license.dat
