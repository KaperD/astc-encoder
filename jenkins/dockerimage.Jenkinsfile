pipeline {
  agent {
    label 'streamline-linux'
  }
  environment {
    ARTIFACTORY_CREDENTIALS = credentials('cepe-artifactory-jenkins')
  }
  options {
    ansiColor('xterm')
    timestamps()
  }
  stages {
    stage('Build and Push Image') {
      steps {
        sh '''
          chmod u+x ./jenkins/build-image.sh
          ./jenkins/build-image.sh push
        '''
      }
    }
  }
}
