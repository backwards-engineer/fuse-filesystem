import jenkins.model.Jenkins
import hudson.security.HudsonPrivateSecurityRealm
import hudson.security.FullControlOnceLoggedInAuthorizationStrategy
import hudson.util.Secret
import com.cloudbees.plugins.credentials.CredentialsScope
import com.cloudbees.plugins.credentials.SystemCredentialsProvider
import com.cloudbees.plugins.credentials.domains.Domain
import com.cloudbees.plugins.credentials.impl.UsernamePasswordCredentialsImpl
import org.jenkinsci.plugins.workflow.job.WorkflowJob
import org.jenkinsci.plugins.workflow.cps.CpsScmFlowDefinition
import hudson.plugins.git.GitSCM
import hudson.plugins.git.UserRemoteConfig
import hudson.plugins.git.BranchSpec
import hudson.triggers.SCMTrigger

def j = Jenkins.get()
def marker = new File(j.rootDir, '.local-setup-complete')
if (!marker.exists()) {
    def realm = new HudsonPrivateSecurityRealm(false)
    realm.createAccount(System.getenv('LOCAL_ADMIN_USER'), System.getenv('LOCAL_JENKINS_PASSWORD'))
    j.setSecurityRealm(realm)
    def auth = new FullControlOnceLoggedInAuthorizationStrategy()
    auth.setAllowAnonymousRead(false)
    j.setAuthorizationStrategy(auth)
    j.setNumExecutors(1)
    j.setLabelString('windows-wsl2')
    j.setSlaveAgentPort(-1)
    jenkins.model.JenkinsLocationConfiguration.get().setUrl('http://127.0.0.1:8080/')
    def credentials = new UsernamePasswordCredentialsImpl(CredentialsScope.GLOBAL,
        'local-gitea', 'Local Gitea checkout', System.getenv('LOCAL_ADMIN_USER'), System.getenv('LOCAL_GITEA_PASSWORD'))
    SystemCredentialsProvider.getInstance().getStore().addCredentials(Domain.global(), credentials)
    def job = j.createProject(WorkflowJob, 'local-fusefs')
    def scm = new GitSCM([new UserRemoteConfig('http://127.0.0.1:3000/chris/local-fusefs.git', null, null, 'local-gitea')],
        [new BranchSpec('*/main')], false, [], null, null, [])
    def definition = new CpsScmFlowDefinition(scm, 'Jenkinsfile')
    definition.setLightweight(true)
    job.setDefinition(definition)
    job.addTrigger(new SCMTrigger('* * * * *'))
    job.setDescription('Build and mount-test the Linux FUSE filesystem in WSL2, archive packages, and deploy to local staging.')
    job.save()
    j.save()
    marker.text = 'Local setup complete. Subsequent restarts preserve UI configuration.\n'
}
