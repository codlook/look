<?php
// Plesk Obsidian MVC controller — the Extensions panel dispatches the UI from here
// (/usr/local/psa/admin/plib/modules/look-lang/controllers/IndexController.php).
// Bridges to the self-contained htdocs/index.php: auto_setup + AJAX (?action)
// + page render (phtml). This keeps a single place for the logic.
class IndexController extends pm_Controller_Action
{
    public function indexAction()
    {
        // htdocs/index.php emits HTML/JSON directly → turn off the Zend layout/view.
        $this->_helper->layout->disableLayout();
        $this->_helper->viewRenderer->setNoRender(true);

        $htdocs = '/usr/local/psa/admin/htdocs/modules/look-lang/index.php';
        if (is_file($htdocs)) {
            include $htdocs;   // auto_setup + (?action ? JSON+exit : phtml render)
        } else {
            echo 'LOOK Language: htdocs/index.php not found.';
        }
    }
}
