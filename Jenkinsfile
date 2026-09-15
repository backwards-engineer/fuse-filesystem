pipeline {
    agent { label 'windows-wsl2' }
    options {
        disableConcurrentBuilds()
        timeout(time: 20, unit: 'MINUTES')
        buildDiscarder(logRotator(numToKeepStr: '20', artifactNumToKeepStr: '10'))
        timestamps()
    }
    triggers { pollSCM('* * * * *') }
    stages {
        stage('Build, test and package on Linux') {
            steps {
                bat 'wsl.exe -d Ubuntu-24.04 -u chris --cd "%WORKSPACE%" -- bash ci/build.sh'
            }
        }
        stage('Archive Linux packages') {
            steps { archiveArtifacts artifacts: 'artifacts/*', fingerprint: true }
        }
        stage('Deploy to local staging') {
            steps {
                bat 'wsl.exe -d Ubuntu-24.04 -u chris --cd "%WORKSPACE%" -- bash ci/deploy.sh'
            }
        }
    }
    post {
        always { junit testResults: 'build/test-results.xml', allowEmptyResults: true }
    }
}
