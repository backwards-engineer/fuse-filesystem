pipeline {
    agent { label 'windows-wsl2' }
    options {
        disableConcurrentBuilds()
        timeout(time: 30, unit: 'MINUTES')
        buildDiscarder(logRotator(numToKeepStr: '20', artifactNumToKeepStr: '10'))
        timestamps()
    }
    triggers { pollSCM('* * * * *') }
    stages {
        stage('Build in WSL2') {
            steps {
                bat 'wsl.exe -d Ubuntu-24.04 -u chris --cd "%WORKSPACE%" -- bash ci/build.sh configure'
            }
        }
        stage('Storage and mounted FUSE tests in WSL2') {
            steps {
                bat 'wsl.exe -d Ubuntu-24.04 -u chris --cd "%WORKSPACE%" -- bash ci/build.sh test'
            }
        }
        stage('Address and undefined-behavior sanitizers in WSL2') {
            steps {
                bat 'wsl.exe -d Ubuntu-24.04 -u chris --cd "%WORKSPACE%" -- bash ci/sanitize.sh'
            }
        }
        stage('Storage and FUSE benchmarks in WSL2') {
            steps {
                bat 'wsl.exe -d Ubuntu-24.04 -u chris --cd "%WORKSPACE%" -- bash ci/build.sh benchmark'
            }
        }
        stage('Package on Linux') {
            steps {
                bat 'wsl.exe -d Ubuntu-24.04 -u chris --cd "%WORKSPACE%" -- bash ci/build.sh package'
            }
        }
        stage('Archive Linux packages') {
            steps { archiveArtifacts artifacts: 'artifacts/*', fingerprint: true }
        }
        stage('Deploy to local staging') {
            when { expression { env.GIT_BRANCH == 'origin/main' } }
            steps {
                bat 'wsl.exe -d Ubuntu-24.04 -u chris --cd "%WORKSPACE%" -- bash ci/deploy.sh'
            }
        }
    }
    post {
        always {
            junit testResults: 'build/*results.xml', allowEmptyResults: true
        }
    }
}
